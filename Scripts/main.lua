local MOD = "AnyPalbook"
local VERSION = "1.0.0"

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
