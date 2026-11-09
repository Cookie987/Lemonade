-- 在电脑上运行：lua host_tests/test_reader_core.lua
package.path = "./lib/?.lua;" .. package.path

local core = require("reader_core")

local passed = 0
local function ok(condition, message)
    if not condition then
        error(message or "assertion failed", 2)
    end
    passed = passed + 1
end

-- UTF-8 ---------------------------------------------------------------
ok(core.is_utf8("hello world"), "ascii should be utf8")
ok(core.is_utf8("中文小说，测试。"), "chinese should be utf8")
ok(not core.is_utf8("\255\254"), "invalid bytes should fail")
ok(not core.is_utf8("hello\0world"), "NUL should fail")
local chinese = "中"
ok(not core.is_utf8(chinese:sub(1, 2)), "partial chinese should fail")
ok(core.is_utf8(chinese:sub(1, 2), true), "partial chinese allowed at end")

ok(core.char_units(65) == 1, "ascii width")
ok(core.char_units(0x4E2D) == 2, "cjk width")
ok(core.char_units(9) == 4, "tab width")

local stripped, bom_len = core.strip_bom("\239\187\191abc")
ok(stripped == "abc" and bom_len == 3, "bom strip")

-- pagination ---------------------------------------------------------
local lines = {}
for i = 1, 20 do
    lines[#lines + 1] = "line"
end
local chunk = table.concat(lines, "\n") .. "\n"
local page, used, filled, line_count = core.paginate(chunk)
ok(line_count == 7, "should fill 7 lines, got " .. line_count)
ok(filled, "page should be filled")
ok(used == 35, "used bytes should be 35, got " .. used)
ok(select(2, page:gsub("\n", "\n")) == 6, "page should have 6 newlines")

local ascii = string.rep("a", 40 * 9)
page, used, filled = core.paginate(ascii)
ok(#page == 36 * 7 + 6, "7 wrapped lines of 36 chars, got " .. #page)
ok(used == 36 * 7, "used " .. used .. " bytes")
ok(filled, "page should be filled after 7 lines")

local cjk = string.rep("中", 19 * 9)
page, used, filled = core.paginate(cjk)
ok(#page == 18 * 7 * 3 + 6, "7 wrapped lines of 18 cjk chars, got bytes " .. #page)
ok(used == 18 * 7 * 3, "used " .. used .. " bytes")
ok(filled, "cjk page should be filled")

local long_line = string.rep("a", 100)
page, used, filled = core.paginate(long_line)
ok(page == string.rep("a", 36) .. "\n" .. string.rep("a", 36) .. "\n" .. string.rep("a", 28),
    "long line should be hard-wrapped")
ok(used == 100, "long line used bytes")
ok(not filled, "100 chars is only 3 lines")

local crlf = "a\r\nb\r\n"
page, used = core.paginate(crlf)
ok(page == "a\nb", "crlf should normalize, got " .. string.format("%q", page))
ok(used == 6, "crlf used bytes")

-- paths --------------------------------------------------------------
ok(core.title_from_path("/sdcard/Books/斗破苍穹.txt") == "斗破苍穹", "title")
ok(core.title_from_path("/sdcard/Books/README.md") == "README.md", "md is not a book")
ok(core.folder_from_path("/sdcard/Books/玄幻/书.txt") == "Books/玄幻", "folder")
ok(core.folder_from_path("/sdcard/书.txt") == "SD 卡", "root folder")
ok(core.is_book_filename("a.TXT"), "txt extension")
ok(not core.is_book_filename("a.md"), "md rejected")
ok(not core.is_book_filename("a.pdf"), "pdf rejected")
ok(not core.is_book_filename(".hidden.txt"), "hidden rejected")

-- progress -----------------------------------------------------------
ok(core.percent(0, 1000) == "0%", "zero percent")
ok(core.percent(500, 1000) == "50%", "half percent")
ok(core.percent(999, 1000) == "100%", "round percent")
ok(core.percent(1, 1000) == "<1%", "small percent")
ok(core.percent_precise(123, 1000) == "12.3%", "precise percent")
ok(core.clamp_offset(-5, 100) == 0, "clamp low")
ok(core.clamp_offset(500, 100) == 100, "clamp high")

print(string.format("reader_core: %d assertions passed", passed))
