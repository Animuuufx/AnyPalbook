local MOD = "AnyPalbook"
local VERSION = "1.2.0"

local function log(msg)
    print(string.format("[%s v%s] %s\n", MOD, VERSION, tostring(msg)))
end

local function script_dir()
    local src = debug.getinfo(1, "S").source or ""
    if src:sub(1, 1) == "@" then
        src = src:sub(2)
    end
    return src:match("^(.*[\\/])") or ""
end

local scripts = script_dir()
local root = scripts:gsub("[Ss]cripts[\\/]$", "")
local dll = root .. "Native\\AnyPalbook.dll"

log("Loading native patch: " .. dll)

local loader, load_err = package.loadlib(dll, "luaopen_anypalbook")
if not loader then
    log("ERROR: could not load AnyPalbook.dll: " .. tostring(load_err))
    log("Check AnyPalbook\\anypalbook.log after installing the compiled DLL.")
    return
end

local ok, call_err = pcall(loader)
if not ok then
    log("ERROR: native initializer threw: " .. tostring(call_err))
    return
end

log("Native initializer executed. Check AnyPalbook\\anypalbook.log for the patch result.")

-- The Pal detail/party UI does not build its work-suitability icons from the
-- single-rank accessors. It asks GetWorkSuitabilityRanksWithCharacterRank for a
-- TMap, and vanilla omits suitability types the species did not originally have.
-- v1.1 already makes the underlying rank checks see handbook-only ranks; this
-- post-hook mirrors those ranks into the returned TMap so the normal UI can draw
-- the icon and level without replacing any widgets.
local WORK_TYPE_COUNT = 13
local ui_logged = {}
local ui_error_logged = false

local function patch_work_suitability_map(Context, ReturnValue)
    local success, err = pcall(function()
        if not Context or not ReturnValue then return end

        local param = Context:get()
        if not param or not param:IsValid() then return end

        local ranks = ReturnValue:get()
        if not ranks then return end

        for work = 1, WORK_TYPE_COUNT do
            local existing = nil
            pcall(function()
                existing = ranks:GetByKey(work)
            end)

            if existing == nil then
                local rank = nil
                pcall(function()
                    rank = param:GetWorkSuitabilityRankWithCharacterRank(work)
                end)
                rank = tonumber(rank)

                if rank and rank > 0 then
                    ranks:Add(work, math.floor(rank))
                    if not ui_logged[work] then
                        ui_logged[work] = true
                        log(string.format("UI: inserted handbook-only suitability type=%d rank=%d into display map.", work, math.floor(rank)))
                    end
                end
            end
        end
    end)

    if not success and not ui_error_logged then
        ui_error_logged = true
        log("ERROR: work-suitability UI map hook failed: " .. tostring(err))
    end
end

local hook_ok, hook_pre, hook_post = pcall(function()
    return RegisterHook(
        "/Script/Pal.PalIndividualCharacterParameter:GetWorkSuitabilityRanksWithCharacterRank",
        function() end,
        patch_work_suitability_map
    )
end)

if hook_ok then
    log(string.format("UI compatibility hook installed (pre=%s post=%s).", tostring(hook_pre), tostring(hook_post)))
else
    log("ERROR: could not install work-suitability UI compatibility hook: " .. tostring(hook_pre))
end
