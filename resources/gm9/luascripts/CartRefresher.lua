-- CartRefresher.lua
--
-- Cartridge Preservation Tool
--
-- Behavior:
--   1. Wait for a 3DS cartridge.
--   2. Verify the FULL .3ds image immediately.
--   3. Wait one hour after with refresh every 5 minutes.
--   4. Repeat until 8 successful verifications.
--   5. Prompt to remove the cartridge, then exit.
--
-- ONE CARTRIDGE PER SCRIPT RUN. Very long-running Lua
-- sessions have proven unstable in this environment, so the
-- script exits when its cartridge is done - completed,
-- failed, or swapped for a different game - instead of
-- looping forever. Run it again for the next cartridge.
-- (Reseating the SAME game mid-run still resumes normally.)
--
-- If verification fails while the same cartridge is still
-- present, the failure is logged and the script exits;
-- run "Fix cartridge corruption" on that game.
--
-- Hold START during a waiting period to exit. Holding B during a
-- verification cancels it and exits the script.
--
-- Log:
--   0:/gm9/out/CartRefresherLog.txt
--
-- Install to 0:/gm9/luascripts/ and run via HOME menu -> Lua scripts...
-- Keep the console connected to a charger.
--
-- Timing / resource notes:
--
-- All waiting is paced by a monotonic clock derived from
-- os.clock() (the free-running ARM9 hardware timer). os.time()
-- is NOT used for pacing: in this build every os.time() call is
-- a full ARM9 -> PXI -> ARM11 -> I2C round trip to the MCU RTC,
-- and the ARM11 I2C driver waits with no timeout - hammering it
-- from a busy loop for hours can hard-freeze the console. The
-- RTC is now only read for log timestamps and one startup clock
-- calibration.
--
-- The status screens and the log include resource snapshots
-- (Lua heap, loop rate, battery) so a leak or slowdown can be
-- diagnosed after an unattended failure.


------------------------------------------------------------
-- Configuration
------------------------------------------------------------

local REQUIRED_PASSES = 8
local VERIFY_INTERVAL = 60 * 60      -- 1 hour
local CART_POLL_INTERVAL = 5         -- check cartridge every 5 secs
local STATUS_UPDATE_INTERVAL = 30    -- refresh status display every 30 sec
local REFRESH_INTERVAL_MINUTES = 5   -- refresh command (0xC5) every 5 mins
local RESOURCE_LOG_INTERVAL = 30 * 60  -- log a resource snapshot every 30 min
local LOG_FILE = GM9OUT .. "/CartRefresherLog.txt"


------------------------------------------------------------
-- Monotonic clock
--
-- os.clock() in this build reads the free-running 64-bit
-- ARM9 hardware timer - no PXI traffic, no I2C traffic, and
-- it cannot return garbage the way a failed RTC read can
-- (os.time() ignores the get_dstime() error and converts
-- whatever bytes were left in the shared PXI buffer).
--
-- This fork's os.clock() also has a unit bug: it divides
-- microseconds by 1e7, so it advances by 1.0 every TEN
-- seconds. Rather than hardcoding that (it may get fixed),
-- the unit is measured once at startup against two RTC
-- second-edges. calibrate_clock() is the only place other
-- than log() that reads the RTC.
------------------------------------------------------------

local CLOCK_FALLBACK_UNITS = 0.1     -- os.clock() units per second
                                     -- as currently implemented

local clock_units_per_sec = CLOCK_FALLBACK_UNITS

local function calibrate_clock()

    local MAX_POLLS = 20000

    local function next_second_edge()
        local t0 = os.time()
        for _ = 1, MAX_POLLS do
            local t = os.time()
            if t ~= t0 then
                return t
            end
        end
        return nil
    end

    local t1 = next_second_edge()
    if not t1 then return false end
    local c1 = os.clock()

    local t2 = next_second_edge()
    if not t2 then return false end
    local c2 = os.clock()

    local dt = t2 - t1
    local dc = c2 - c1

    if dt >= 1 and dt <= 3 and dc > 0 then
        local units = dc / dt
        if units > 0.0005 and units < 50 then
            clock_units_per_sec = units
            return true
        end
    end

    return false
end

local mono_last = 0

local function mono_raw()
    return os.clock() / clock_units_per_sec
end

-- Monotonic seconds since script start.
--
-- The hardware timer is read as four separate 16-bit
-- registers, so a read can rarely be "torn" across a carry
-- and jump wildly for one sample. Two consecutive reads are
-- only microseconds apart, so require two samples that
-- agree; a single wild sample gets discarded. The result is
-- also clamped to never run backwards.
local function mono_now()

    local a = mono_raw()

    for _ = 1, 4 do
        local b = mono_raw()
        local d = b - a
        a = b
        if d >= 0 and d < 2 then
            break
        end
    end

    if a > mono_last then
        mono_last = a
    end

    return mono_last
end


------------------------------------------------------------
-- Resource monitoring
--
-- There is no OS underneath GodMode9, so there is no "CPU
-- usage" to query - the ARM9 always runs flat out. The
-- closest useful signal is the wait-loop iteration rate: if
-- it decays over the hours, something is degrading.
--
-- Lua heap usage (collectgarbage) is the number that grows
-- if this script leaks memory. A full GC runs before each
-- measurement so the value is live data, not garbage
-- awaiting collection.
------------------------------------------------------------

local perf = {
    iters = 0,          -- total wait-loop iterations
    last_iters = 0,
    last_time = nil,
    rate = nil,         -- iterations per second, nil = unknown
    mem_kb = 0,
    peak_kb = 0,
    start_kb = nil,
}

local function perf_tick()
    perf.iters = perf.iters + 1
end

local function update_resources(now)

    if perf.last_time and now > perf.last_time then
        local elapsed = now - perf.last_time
        if elapsed <= 2 * STATUS_UPDATE_INTERVAL then
            perf.rate = (perf.iters - perf.last_iters) / elapsed
        else
            perf.rate = nil
        end
    end

    perf.last_iters = perf.iters
    perf.last_time = now

    collectgarbage("collect")
    perf.mem_kb = collectgarbage("count")

    if not perf.start_kb then
        perf.start_kb = perf.mem_kb
    end

    if perf.mem_kb > perf.peak_kb then
        perf.peak_kb = perf.mem_kb
    end
end

-- Battery percent with charge state:
--   "87%+"  charging
--   "87%="  on the adapter, not charging (i.e. full)
--   "87%-"  RUNNING ON BATTERY - the charger fell out!
--
-- Two small MCU reads per status update (30 s) is harmless;
-- it is the multi-kHz hammering that had to go.
local function battery_status()

    if not i2c then
        return "?"
    end

    local ok, pct = pcall(
        i2c.read, i2c.dev.MCU,
        i2c.mcu.reg.BATTERY_PERCENTAGE_INT, 1)

    if not ok or type(pct) ~= "table" or not pct[1] then
        return "?"
    end

    local suffix = "?"

    local ok2, pstat = pcall(
        i2c.read, i2c.dev.MCU,
        i2c.mcu.reg.POWER_STATUS, 1)

    if ok2 and type(pstat) == "table" and pstat[1] then
        local flags = pstat[1]
        if (flags & i2c.mcu.power_status_flags.CHARGING) ~= 0 then
            suffix = "+"
        elseif (flags & i2c.mcu.power_status_flags.ADAPTER_CONNECTED) ~= 0 then
            suffix = "="
        else
            suffix = "-"
        end
    end

    return tostring(pct[1]) .. "%" .. suffix
end

local function format_rate(rate)
    if not rate then
        return "?"
    end
    if rate >= 1000 then
        return string.format("%.1fk/s", rate / 1000)
    end
    return string.format("%.0f/s", rate)
end

local function format_uptime(now)
    local s = math.floor(now)
    return string.format("%dh%02dm", s // 3600, (s % 3600) // 60)
end

local function resource_text(now)
    return string.format(
        "Mem %dK start %dK peak %dK\nBatt %s  Loop %s  Up %s",
        math.floor(perf.mem_kb + 0.5),
        math.floor((perf.start_kb or 0) + 0.5),
        math.floor(perf.peak_kb + 0.5),
        battery_status(),
        format_rate(perf.rate),
        format_uptime(now))
end

-- log_resources is defined below, after log() exists.
local log_resources

------------------------------------------------------------
-- Logging
------------------------------------------------------------

local function log(message)
    local file = io.open(LOG_FILE, "a")

    if not file then
        -- Logging failure should never stop cartridge verification.
        return
    end

    -- Do not use os.date() with a format string here: this fork's
    -- strftime implementation prints minutes for %S and mis-pads %H
    -- (with a small buffer overflow). The "*t" table path reads the
    -- RTC fields directly and is safe.
    local t = os.date("*t")

    -- An SD hiccup during an unattended write must not kill
    -- the whole script with a Lua error.
    pcall(function()
        file:write(
            string.format(
                "[%04d-%02d-%02d %02d:%02d:%02d] ",
                t.year, t.month, t.day, t.hour, t.min, t.sec
            )
            .. message
            .. "\n"
        )
    end)

    pcall(file.close, file)
end

log_resources = function(now, context)
    log(string.format(
        "RES %s up=%s mem=%.0fKB start=%.0fKB peak=%.0fKB loop=%s batt=%s",
        context,
        format_uptime(now),
        perf.mem_kb,
        perf.start_kb or 0,
        perf.peak_kb,
        format_rate(perf.rate),
        battery_status()))
end


------------------------------------------------------------
-- Find cartridge
--
-- Returns:
--
--   path, "3ds"       3DS cartridge
--   path, "nds"       DS / DSi cartridge
--   nil,  "none"      nothing detected
--   nil,  "toobig"    3DS cart with no verifiable single image
--   nil,  "unknown"   C: exists but no recognized cart image
------------------------------------------------------------

local function find_cartridge()

    -- C: may disappear / fail when no cartridge is inserted.
    -- fs.list_dir() is documented as throwing on directory errors,
    -- so pcall is appropriate here.
    local ok, entries = pcall(fs.list_dir, "C:/")

    if not ok or not entries then
        return nil, "none"
    end

    local nds_path = nil
    local trim_path = nil
    local split_seen = false

    for _, entry in ipairs(entries) do

        if entry.type == "file" then

            local lower = string.lower(entry.name)

            ------------------------------------------------
            -- Prefer the full 3DS image, fall back to the
            -- trimmed one. fs.verify() walks the NCSD
            -- partition hashes either way, so full vs trim
            -- makes no difference to what gets read - but
            -- on carts whose data fills the entire 4 GiB
            -- chip, C: exposes neither image (only
            -- .split.000/.001, which cannot be verified).
            ------------------------------------------------

            if string.match(lower, "%.trim%.3ds$") then
                trim_path = "C:/" .. entry.name

            elseif string.match(lower, "%.3ds$") then
                return "C:/" .. entry.name, "3ds"

            elseif string.match(lower, "%.split%.000$") then
                split_seen = true

            ------------------------------------------------
            -- Remember DS/DSi cart so we can give the user
            -- a useful message if no .3ds image exists.
            ------------------------------------------------

            elseif string.match(lower, "%.nds$") then
                nds_path = "C:/" .. entry.name
            end
        end
    end

    if trim_path then
        return trim_path, "3ds"
    end

    if split_seen then
        return nil, "toobig"
    end

    if nds_path then
        return nds_path, "nds"
    end

    return nil, "unknown"
end


------------------------------------------------------------
-- Identify a game
--
-- We read the first 0x200 bytes of the full .3ds image and
-- SHA-256 hash them.
--
-- This gives us a stable game/image identifier without
-- reading the entire cartridge just to identify it.
--
-- The identifier is kept as the raw SHA-256 byte string.
------------------------------------------------------------

local function identify_game(path)

    -- fs.read_file() is documented as throwing on read errors.
    local ok, header = pcall(fs.read_file, path, 0, 0x200)

    if not ok or not header then
        return nil
    end

    return fs.hash_data(header)
end


------------------------------------------------------------
-- Get currently inserted 3DS cartridge + identity
--
-- Returns:
--
--   path, id, "3ds"
--   nil, nil, "none"
--   nil, nil, "nds"
--   nil, nil, "unknown"
--   nil, nil, "read_error"
------------------------------------------------------------

local function get_current_cartridge()

    local path, kind = find_cartridge()

    if kind ~= "3ds" then
        return nil, nil, kind
    end

    local id = identify_game(path)

    if not id then
        return nil, nil, "read_error"
    end

    return path, id, "3ds"
end


------------------------------------------------------------
-- Wait for cartridge
--
-- Used at startup and when the cartridge is reseated
-- mid-run. The caller decides whether the game that comes
-- back is acceptable.
------------------------------------------------------------

local function wait_for_cartridge()

    local next_poll = 0
    local next_status = 0
    local next_resource_log = mono_now() + RESOURCE_LOG_INTERVAL

    local base_message = nil
    local res_text = ""
    local dirty = false

    while true do

        perf_tick()

        ----------------------------------------------------
        -- Exit
        ----------------------------------------------------

        if ui.check_key("START") then
            return nil, nil, "exit"
        end

        local now = mono_now()

        ----------------------------------------------------
        -- Don't repeatedly hammer C:
        ----------------------------------------------------

        if now >= next_poll then

            local path, id, kind = get_current_cartridge()
            local message

            if kind == "3ds" and path and id then

                return path, id, "ok"

            elseif kind == "nds" then

                message =
                    "CartRefresher\n\n"
                    .. "DS / DSi cartridge detected.\n\n"
                    .. "Only Nintendo 3DS cartridges can\n"
                    .. "be verified by this script.\n\n"
                    .. "Insert a 3DS cartridge.\n\n"
                    .. "Hold START to exit."

            elseif kind == "toobig" then

                message =
                    "CartRefresher\n\n"
                    .. "This cartridge's data fills the whole\n"
                    .. "4 GiB chip, so GodMode9 cannot expose\n"
                    .. "a single verifiable image for it.\n\n"
                    .. "Insert a different game.\n\n"
                    .. "Hold START to exit."

            elseif kind == "read_error" then

                message =
                    "CartRefresher\n\n"
                    .. "A 3DS cartridge was detected,\n"
                    .. "but its header could not be read.\n\n"
                    .. "Try removing and reinserting it.\n\n"
                    .. "Hold START to exit."

            else

                message =
                    "CartRefresher\n\n"
                    .. "Waiting for a 3DS cartridge...\n\n"
                    .. "Insert a game to begin.\n\n"
                    .. "Hold START to exit."
            end

            if message ~= base_message then
                base_message = message
                dirty = true
            end

            next_poll = now + CART_POLL_INTERVAL
        end

        ----------------------------------------------------
        -- Periodically refresh the resource display
        ----------------------------------------------------

        if now >= next_status then
            update_resources(now)
            res_text = resource_text(now)
            next_status = now + STATUS_UPDATE_INTERVAL
            dirty = true
        end

        ----------------------------------------------------
        -- Avoid unnecessarily redrawing identical text
        -- (and avoid rebuilding the string every spin)
        ----------------------------------------------------

        if dirty and base_message then
            ui.show_text(base_message .. "\n\n" .. res_text)
            dirty = false
        end

        ----------------------------------------------------
        -- Periodic resource snapshot for the log, so an
        -- unattended failure leaves a trail.
        ----------------------------------------------------

        if now >= next_resource_log then
            log_resources(now, "waiting-for-cart")
            next_resource_log = now + RESOURCE_LOG_INTERVAL
        end
    end
end


------------------------------------------------------------
-- Wait one hour between successful passes
--
-- During the wait we also check whether:
--
--   * the cartridge disappeared
--   * a different cartridge was inserted
--   * START was held
--
-- Returns:
--
--   "time"
--   "removed"
--   "changed", new_path, new_id
--   "exit"
------------------------------------------------------------

local function wait_for_next_pass(expected_id, completed_passes, path)

    -- Interval starts AFTER the previous verification finishes.
    local deadline = mono_now() + VERIFY_INTERVAL

    local next_cart_poll = 0
    local next_status_update = 0
    local next_resource_log = mono_now() + RESOURCE_LOG_INTERVAL

    -- First periodic refresh fires one interval from now; a refresh
    -- was already sent right after the verify finished.
    local next_refresh = mono_now() + REFRESH_INTERVAL_MINUTES * 60

    while true do

        perf_tick()

        if ui.check_key("START") then
            return "exit"
        end

        local now = mono_now()

        ----------------------------------------------------
        -- Hour elapsed
        ----------------------------------------------------

        if now >= deadline then
            return "time"
        end

        ----------------------------------------------------
        -- Periodically check the cartridge
        ----------------------------------------------------

        if now >= next_cart_poll then

            local path, id, kind = get_current_cartridge()

            if kind ~= "3ds" or not path or not id then

                return "removed"

            elseif id ~= expected_id then

                return "changed", path, id
            end

            next_cart_poll = now + CART_POLL_INTERVAL
        end

        ----------------------------------------------------
        -- Periodically send the cartridge a refresh
        -- command while idle (this fork only)
        ----------------------------------------------------

        if fs.cart_refresh and now >= next_refresh then

            local ok, sent = pcall(fs.cart_refresh, path)

            if not (ok and sent) then
                -- Cart missing or not a 3DS cart; the 5-second
                -- cartridge poll above handles the consequences.
                log("Cart refresh command failed (cart missing?)")
            end

            next_refresh = now + REFRESH_INTERVAL_MINUTES * 60
        end

        ----------------------------------------------------
        -- Periodically update screen
        ----------------------------------------------------

        if now >= next_status_update then

            local remaining = deadline - now

            if remaining < 0 then
                remaining = 0
            end

            local minutes = math.ceil(remaining / 60)

            update_resources(now)

            ui.show_text(
                "CartRefresher\n\n"
                .. tostring(completed_passes)
                .. " / "
                .. tostring(REQUIRED_PASSES)
                .. " verifications complete.\n\n"
                .. "Next verification in about "
                .. tostring(minutes)
                .. " minute"
                .. (minutes == 1 and "." or "s.")
                .. "\n\n"
                .. "You may leave the console unattended.\n\n"
                .. "Hold START to exit.\n\n"
                .. resource_text(now)
            )

            next_status_update =
                now + STATUS_UPDATE_INTERVAL
        end

        ----------------------------------------------------
        -- Periodic resource snapshot for the log
        ----------------------------------------------------

        if now >= next_resource_log then
            log_resources(now, "hourly-wait")
            next_resource_log = now + RESOURCE_LOG_INTERVAL
        end
    end
end


------------------------------------------------------------
-- End-of-run prompt
--
-- Every way a run ends (except a START / B abort) funnels
-- through here before the script returns.
------------------------------------------------------------

local function exit_prompt(message)
    ui.echo(
        "CartRefresher\n\n"
        .. message
        .. "\n\n"
        .. "This script processes one cartridge per run.\n"
        .. "Press A to close it, then run it again\n"
        .. "for the next cartridge."
    )
end

local DIFFERENT_GAME =
    "A DIFFERENT game was inserted before\n"
    .. "the previous one finished verifying.\n\n"
    .. "That run is abandoned."


------------------------------------------------------------
-- Prepare output directory
------------------------------------------------------------

if not fs.exists(GM9OUT) then
    pcall(fs.mkdir, GM9OUT)
end


------------------------------------------------------------
-- Startup confirmation
------------------------------------------------------------

if not ui.ask(
    "CartRefresher\n\n"
    .. "Each 3DS cartridge will be verified\n"
    .. "8 times using the FULL .3ds image.\n\n"
    .. "The first verification starts immediately.\n"
    .. "Later passes begin one hour after the\n"
    .. "previous pass finishes.\n\n"
    .. "One cartridge per run: after 8 successful\n"
    .. "passes the script prompts and exits.\n\n"
    .. "Keep the console connected to power.\n\n"
    .. "Start?"
) then
    return
end


------------------------------------------------------------
-- Clock calibration
--
-- Measures os.clock() units against two RTC second-edges.
-- Takes one to two seconds. If the RTC misbehaves, retry
-- once, then fall back to the factor this build is known
-- to use (and say so in the log).
------------------------------------------------------------

ui.show_text(
    "CartRefresher\n\n"
    .. "Calibrating monotonic clock...\n\n"
    .. "This takes a few seconds."
)

local clock_calibrated = calibrate_clock() or calibrate_clock()

update_resources(mono_now())

log(string.format(
    "CartRefresher started (os.clock units/sec: %.6f%s)",
    clock_units_per_sec,
    clock_calibrated and "" or " - CALIBRATION FAILED, using fallback"))

log_resources(mono_now(), "startup")


------------------------------------------------------------
-- Initial cartridge
------------------------------------------------------------

local cart_path, cart_id, result =
    wait_for_cartridge()

if result == "exit" then
    log("CartRefresher stopped by user")
    return
end


------------------------------------------------------------
-- Process this run's single cartridge
------------------------------------------------------------

local successes = 0

log("New cartridge: " .. cart_path)

do
    while successes < REQUIRED_PASSES do

        ----------------------------------------------------
        -- Confirm the expected cartridge is still present
        -- immediately before starting another full verify.
        ----------------------------------------------------

        local detected_path, detected_id, detected_kind =
            get_current_cartridge()

        if detected_kind ~= "3ds"
            or not detected_path
            or not detected_id then

            ------------------------------------------------
            -- Cartridge disappeared.
            ------------------------------------------------

            log(
                "Cartridge removed with "
                .. tostring(successes)
                .. "/"
                .. tostring(REQUIRED_PASSES)
                .. " successful passes: "
                .. cart_path
            )

            local new_path, new_id, wait_result =
                wait_for_cartridge()

            if wait_result == "exit" then
                log("CartRefresher stopped by user")
                return
            end

            ------------------------------------------------
            -- Same game reinserted:
            -- retain its completed pass count.
            --
            -- Different game: this run is over.
            ------------------------------------------------

            if new_id ~= cart_id then

                log(
                    "Different cartridge inserted before cycle completed: "
                    .. cart_path
                    .. " -> "
                    .. new_path
                )

                exit_prompt(DIFFERENT_GAME)
                return

            else
                cart_path = new_path

                log(
                    "Same cartridge reinserted; resuming at "
                    .. tostring(successes)
                    .. "/"
                    .. tostring(REQUIRED_PASSES)
                    .. ": "
                    .. cart_path
                )
            end

        elseif detected_id ~= cart_id then

            ------------------------------------------------
            -- Cartridge changed without us first observing
            -- an empty slot.
            ------------------------------------------------

            log(
                "Cartridge swapped before cycle completed: "
                .. cart_path
                .. " -> "
                .. detected_path
            )

            exit_prompt(DIFFERENT_GAME)
            return

        else

            -- Use the freshly detected path.
            cart_path = detected_path

            ------------------------------------------------
            -- Run verification
            ------------------------------------------------

            local pass_number = successes + 1

            ui.show_text(
                "CartRefresher\n\n"
                .. "Running verification "
                .. tostring(pass_number)
                .. " / "
                .. tostring(REQUIRED_PASSES)
                .. "...\n\n"
                .. cart_path
                .. "\n\n"
                .. "Do not remove the cartridge."
            )

            ------------------------------------------------
            -- fs.verify() already returns true / false.
            -- No pcall wrapper is necessary here.
            ------------------------------------------------

            local verified = fs.verify(cart_path)

            ------------------------------------------------
            -- The verify progress bar would otherwise stay
            -- on screen stuck near 99% (its final frame
            -- falls victim to draw throttling). Passing the
            -- verified path to fs.cart_refresh repaints the
            -- bar as finished (100%), sends the cart a
            -- refresh command right away and redraws the
            -- refresh counter. On builds without the
            -- binding the stale bar simply stays.
            ------------------------------------------------

            if fs.cart_refresh then
                pcall(fs.cart_refresh, cart_path)
            end

            ------------------------------------------------
            -- Resource snapshot after every verification;
            -- eight per day of these plus the 30-minute
            -- lines give a trend to read after a failure.
            ------------------------------------------------

            update_resources(mono_now())
            log_resources(mono_now(), "after-verify")

            if verified then

                successes = successes + 1

                log(
                    "Verify OK ("
                    .. tostring(successes)
                    .. "/"
                    .. tostring(REQUIRED_PASSES)
                    .. "): "
                    .. cart_path
                )

                ------------------------------------------------
                -- Finished all eight
                ------------------------------------------------

                if successes >= REQUIRED_PASSES then

                    log(
                        "COMPLETE - "
                        .. tostring(REQUIRED_PASSES)
                        .. "/"
                        .. tostring(REQUIRED_PASSES)
                        .. " successful verifications: "
                        .. cart_path
                    )

                    log_resources(mono_now(), "complete")

                    exit_prompt(
                        "All "
                        .. tostring(REQUIRED_PASSES)
                        .. " verifications PASSED:\n\n"
                        .. cart_path
                        .. "\n\n"
                        .. "Remove the cartridge."
                    )

                    return
                end

                ------------------------------------------------
                -- Wait one hour
                ------------------------------------------------

                local event, new_path, new_id =
                    wait_for_next_pass(cart_id, successes, cart_path)

                if event == "exit" then

                    log("CartRefresher stopped by user")
                    return

                elseif event == "changed" then

                    log(
                        "Cartridge changed during hourly wait: "
                        .. cart_path
                        .. " -> "
                        .. new_path
                    )

                    exit_prompt(DIFFERENT_GAME)
                    return

                elseif event == "removed" then

                    log(
                        "Cartridge removed during hourly wait at "
                        .. tostring(successes)
                        .. "/"
                        .. tostring(REQUIRED_PASSES)
                        .. ": "
                        .. cart_path
                    )

                    local old_id = cart_id

                    local replacement_path,
                          replacement_id,
                          wait_result =
                        wait_for_cartridge()

                    if wait_result == "exit" then
                        log("CartRefresher stopped by user")
                        return
                    end

                    if replacement_id == old_id then

                        ------------------------------------------------
                        -- Same game came back.
                        -- Keep the existing pass count.
                        ------------------------------------------------

                        cart_path = replacement_path

                        log(
                            "Same cartridge reinserted; "
                            .. "resuming at "
                            .. tostring(successes)
                            .. "/"
                            .. tostring(REQUIRED_PASSES)
                            .. ": "
                            .. cart_path
                        )

                    else

                        ------------------------------------------------
                        -- Different game: this run is over.
                        ------------------------------------------------

                        log(
                            "Different cartridge inserted: "
                            .. cart_path
                            .. " -> "
                            .. replacement_path
                        )

                        exit_prompt(DIFFERENT_GAME)
                        return
                    end
                end

            else

                ------------------------------------------------
                -- Verification returned false.
                --
                -- Pressing B cancels the verification progress
                -- bar, which also lands here. Check for it
                -- first so a user abort is not logged as a
                -- cartridge failure. (Keep holding B until the
                -- script exits.)
                ------------------------------------------------

                if ui.check_key("B") then
                    log("Verification canceled by user")
                    return
                end

                ------------------------------------------------
                -- Otherwise determine whether this was simply
                -- because the cart disappeared / changed.
                ------------------------------------------------

                local after_path, after_id, after_kind =
                    get_current_cartridge()

                if after_kind ~= "3ds"
                    or not after_path
                    or not after_id then

                    log(
                        "Verification interrupted by cartridge removal: "
                        .. cart_path
                    )

                    local replacement_path,
                          replacement_id,
                          wait_result =
                        wait_for_cartridge()

                    if wait_result == "exit" then
                        log("CartRefresher stopped by user")
                        return
                    end

                    if replacement_id ~= cart_id then

                        log(
                            "Different cartridge inserted: "
                            .. cart_path
                            .. " -> "
                            .. replacement_path
                        )

                        exit_prompt(DIFFERENT_GAME)
                        return
                    end

                    cart_path = replacement_path

                elseif after_id ~= cart_id then

                    ------------------------------------------------
                    -- Swapped during verification.
                    ------------------------------------------------

                    log(
                        "Cartridge changed during verification: "
                        .. cart_path
                        .. " -> "
                        .. after_path
                    )

                    exit_prompt(DIFFERENT_GAME)
                    return

                else

                    ------------------------------------------------
                    -- Same cartridge is still inserted.
                    --
                    -- Treat this as a genuine verification
                    -- failure and DO NOT keep hammering it.
                    ------------------------------------------------

                    log(
                        "VERIFY FAILED after "
                        .. tostring(successes)
                        .. "/"
                        .. tostring(REQUIRED_PASSES)
                        .. " successful passes: "
                        .. cart_path
                    )

                    log_resources(mono_now(), "verify-failed")

                    ------------------------------------------------
                    -- The Cartridge Fixer README says a failed
                    -- verification is the point where its
                    -- corruption-fixing operation should be used.
                    --
                    -- Therefore this preservation script does not
                    -- automatically retry a failing cartridge.
                    ------------------------------------------------

                    exit_prompt(
                        "VERIFY FAILED after "
                        .. tostring(successes)
                        .. "/"
                        .. tostring(REQUIRED_PASSES)
                        .. " successful passes:\n\n"
                        .. cart_path
                        .. "\n\n"
                        .. "Run Fix cartridge corruption\n"
                        .. "on this game."
                    )

                    return
                end
            end
        end
    end
end
