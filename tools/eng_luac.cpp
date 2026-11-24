// eng-luac <src-dir> <out-dir>
//
// Compiles every .lua under src-dir to bytecode at the same relative path in
// out-dir (ai/hound.lua → ai/hound.luac): what luac does, for a whole tree,
// from the same Lua sources the runtime is built from. Debug info is kept so
// release tracebacks still have file:line. Chunk names are "@<relative path>",
// matching what eng::Scripts uses when it loads the source in dev.
// Reports every syntax error, then exits non-zero if there was any.

#include <lua/lauxlib.h>
#include <lua/lua.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int writer(lua_State*, const void* p, size_t size, void* ud) {
    static_cast<std::string*>(ud)->append(static_cast<const char*>(p), size);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: eng-luac <src-dir> <out-dir>\n");
        return 2;
    }
    fs::path src = argv[1], out = argv[2];
    std::error_code ec;
    if (!fs::is_directory(src, ec)) {
        std::fprintf(stderr, "eng-luac: %s is not a directory\n", argv[1]);
        return 2;
    }

    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(src))
        if (entry.is_regular_file() && entry.path().extension() == ".lua") files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    lua_State* L = luaL_newstate();
    int errors = 0;
    for (const fs::path& file : files) {
        std::string rel = fs::relative(file, src).generic_string();
        std::ifstream in(file, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string code = std::move(ss).str();
        std::string chunkname = "@" + rel;
        if (luaL_loadbufferx(L, code.data(), code.size(), chunkname.c_str(), "t") != LUA_OK) {
            std::fprintf(stderr, "%s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            ++errors;
            continue;
        }
        std::string bytecode;
        lua_dump(L, writer, &bytecode, 0);
        lua_pop(L, 1);

        fs::path dst = out / rel;
        dst.replace_extension(".luac");
        fs::create_directories(dst.parent_path());
        std::ofstream o(dst, std::ios::binary);
        o.write(bytecode.data(), static_cast<std::streamsize>(bytecode.size()));
        if (!o) {
            std::fprintf(stderr, "eng-luac: couldn't write %s\n", dst.string().c_str());
            ++errors;
        }
    }
    lua_close(L);
    fs::create_directories(out);  // an empty tree is still an output
    if (errors) std::fprintf(stderr, "eng-luac: %d error(s)\n", errors);
    return errors ? 1 : 0;
}
