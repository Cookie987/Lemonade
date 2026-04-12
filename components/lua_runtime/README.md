# Lua Runtime (ESPHome External Component)

This component lets the firmware load and run Lua scripts from the filesystem
(e.g. SD card mounted at `/sdcard`).

## How it works
- Adds a `lua_runtime` component and an action `lua_runtime.run_file`.
- Reads the script via standard VFS (`std::ifstream`).
- Executes the script in a fresh Lua VM.
- Registers built-in APIs such as `log(...)` and `switch.get_state(...)`.

## Important: add Lua sources
This repo does not include Lua sources. To enable the runtime:
1. Download Lua 5.4.x source from the official Lua site.
2. Copy the `src` files into:
   `components/lua_runtime/lua/`
3. Ensure `lua.hpp` is present in that folder.
4. In YAML, set `enable_stub: false` for the component.

When `enable_stub: true` (default), the component builds but scripts will not run.

## YAML usage
```yaml
lua_runtime:
  id: lua
  enable_stub: true  # set to false after adding Lua sources

script:
  - id: run_sd_lua
    then:
      - lua_runtime.run_file:
          id: lua
          path: "/sdcard/apps/hello.lua"
```

## Example Lua script
```lua
log("Hello from Lua on SD!")

local wifi_on, err = switch.get_state("wifi_switch")
if wifi_on == nil then
  log.warn("lua", err)
else
  log.info("lua", "wifi_switch =", wifi_on)
end
```
