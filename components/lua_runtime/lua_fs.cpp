#include "lua_fs.h"

#ifdef LUA_RUNTIME_STUB
struct lua_State;

namespace esphome {
namespace lua_runtime {

void register_fs_api(lua_State *) {}

}  // namespace lua_runtime
}  // namespace esphome

#else

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>

#include "lua.hpp"

namespace esphome {
namespace lua_runtime {

static bool stat_path(const char *path, struct stat *st) { return path != nullptr && stat(path, st) == 0; }

static int lua_fs_exists(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  struct stat st;
  lua_pushboolean(L, stat_path(path, &st));
  return 1;
}

static int lua_fs_isdir(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  struct stat st;
  lua_pushboolean(L, stat_path(path, &st) && S_ISDIR(st.st_mode));
  return 1;
}

static int lua_fs_listdir(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  DIR *dir = opendir(path);
  if (dir == nullptr) {
    lua_pushnil(L);
    lua_pushfstring(L, "%s", strerror(errno));
    return 2;
  }

  lua_newtable(L);
  int index = 1;
  while (dirent *entry = readdir(dir)) {
    const char *name = entry->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    lua_pushstring(L, name);
    lua_rawseti(L, -2, index++);
  }
  closedir(dir);
  return 1;
}

void register_fs_api(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, lua_fs_exists);
  lua_setfield(L, -2, "exists");
  lua_pushcfunction(L, lua_fs_isdir);
  lua_setfield(L, -2, "isdir");
  lua_pushcfunction(L, lua_fs_listdir);
  lua_setfield(L, -2, "listdir");
  lua_setglobal(L, "fs");
}

}  // namespace lua_runtime
}  // namespace esphome

#endif
