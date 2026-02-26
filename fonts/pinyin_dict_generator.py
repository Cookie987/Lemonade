input_file = "fonts/ime_pinyin_source.txt"
output_file = "fonts/ime_pinyin_output.c"
from pypinyin import lazy_pinyin
from collections import defaultdict


with open(input_file, "r", encoding="utf-8") as f:
    text = f.read()

# 去重
unique_chars = set(text.strip())

# 拼音分组
pinyin_map = defaultdict(list)

for ch in unique_chars:
    if ch.strip():
        py = lazy_pinyin(ch)[0]
        pinyin_map[py].append(ch)

# 排序拼音
sorted_pinyin = sorted(pinyin_map.keys())

with open(output_file, "w", encoding="utf-8") as f:
    f.write("static const pinyin_entry_t pinyin_dict[] = {\n")

    for py in sorted_pinyin:
        # 同拼音汉字排序（可选）
        chars = ''.join(sorted(pinyin_map[py]))
        f.write(f'    {{"{py}", "{chars}"}},\n')

    f.write("    {NULL, NULL}\n};\n")

print("生成完成")