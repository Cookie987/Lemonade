# LVGL 拼音输入法智能化改造方案

> 状态：设计稿（未实施）。当前输入法的「不智能」体感来自四个独立问题，
> 本文先还原现状，再按性价比给出四个改进方向的分析与伪代码。

## 1. 现状梳理

固件里实际生效的输入法**不是** LVGL 上游的 `lv_ime_pinyin` extra 插件
（`anyui_s3_t_24/lvgl/src/extra/others/ime/`，那是旧拷贝），而是本仓库
LVGL fork（`Cookie987/lvgl` 9.5.0，managed component）在 `lv_keyboard`
里内置的中文模式。聊天页键盘按「英」切到的就是 `LV_KEYBOARD_MODE_IME_CHN`。

### 1.1 关键文件与位置

| 内容 | 位置 |
| --- | --- |
| IME 逻辑主体 | `managed_components/lvgl__lvgl/src/widgets/keyboard/lv_keyboard.c`（~2200 行，fork 分叉） |
| 组合状态 | `lv_keyboard_private.h` 的 `struct _lv_keyboard_t`：`pinyin_buf[16]`、`pinyin_start_pos`、`candidates_list`、`candidate_page`（存的是候选列表起始下标）等 |
| 单字字库 | `src/widgets/keyboard/lv_ime_dict.h`：`pinyin_dict[]`，约 430 个音节 → 汉字串（按字库顺序排，无字频字段） |
| 词组词库 | `src/widgets/keyboard/lv_ime_phrase_dict.h`：`ime_phrase_dict[]`，「Auto-generated from Rime dict」，共 4421 条（二字词 3488、三字词 794、≥4 字 136），字段 `pinyin / initials(简拼) / phrase / freq(静态)` |
| 候选条 UI | fork 给 `lv_keyboard` 加的 `lv_keyboard_set_candidate_list(kb, container)`，候选 btnm 动态建在宿主提供的容器里；聊天页在 `packages/s3_touch/features/message/message.yaml` on_boot(399) 里把 `obj_message_candidate_list` 挂给 `keyboard_message_input` |

### 1.2 输入流程（lv_keyboard.c）

1. 字母键 → `lv_keyboard_ime_append_letter()`：拼音追加进 `pinyin_buf`，同时把字母**原样混排**进 textarea（占位），然后 `lv_keyboard_show_candidates()`。
2. `lv_keyboard_collect_phrase_candidates()`（:1809）两路合并候选（上限 100）：
   - **词组路**：`lv_keyboard_collect_rime_candidates()` 拿**整串拼音**精确/前缀/简拼匹配词库 trie，打分 `lv_keyboard_phrase_score()`（:1872）：全拼命中 `3e9+freq`、前缀 `2e9+freq`、简拼 `1e9+freq`，纯静态排序；
   - **单字路**：`lv_keyboard_collect_char_candidates()` → `lv_keyboard_build_candidates()`（:1671）对 `pinyin_dict` 做贪心最长匹配 + DFS 穷举组字，访问预算 512（`LV_KEYBOARD_BUILD_VISIT_BUDGET`），**组合单元只有单字**。
3. 数字键 1-9 选当前页候选、`<` `>` 翻页、空格/标点 = 提交第一个候选（`lv_keyboard_handle_ime_chn()`，:765）。
4. 提交后 `lv_keyboard_ime_reset()` 清空全部状态，**无任何学习/记忆**（全文件无 learn / freq 调整 / 持久化逻辑）。

## 2. 问题诊断

| # | 问题 | 影响 | 定位 |
| --- | --- | --- | --- |
| P0 | **数字键翻页选词错位（真 bug）**：`absolute_idx = candidate_page * 8 + 本页序号`，但 `candidate_page` 是列表起始下标不是页号。翻页后起始下标不是 8 的倍数（词长不同的页大小是 8/5/4/3），按数字键会选到错误候选 | 日常选词直接出错，体感差的主因之一 | `lv_keyboard.c:782-787` |
| P1 | **整句必须拆词**：词组候选要求整串拼音完整命中词库条目；词库里没有的连续输入（如 woyaochifan）退化成单字 DFS，排序是各音节字表顺序的笛卡尔积，目标词沉底 | 只能「一个词一个词」打 | `lv_keyboard_build_candidates()` 只用单字表 |
| P1.1 | **逐字候选大量是瞎组合**：单字路对每个音节独立枚举字库里**全部**字（`lv_keyboard_build_candidates` 逐字符展开 `chinese` 串），只按「拼音切分长度优先」排序，**没有任何字频/词感权重**。「woyao」给出「卧窑哟腰…」这类组合 —— 因为「卧/窑/哟」在各自音节表里靠前，而「要」在 yao 字表里排得很后（见 §2.1）。字库字序按编码/字数排，**不是按常用度** | 翻很多页全是无意义组合，想打的词沉底 | `lv_ime_dict.h` 无频率字段；`lv_keyboard_build_candidates` 无评分剪枝 |
| P2 | **无自学习、无上下文**：每次提交后状态清零，词频是生成词库时固化的静态值，常用词永远排不到前面 | 高频个人词组（人名、口头禅）反复翻页 | 无任何持久化代码 |
| P3 | **词库太小**：3305 个二字词拼音覆盖，口语化常用词大量缺失 | 缺词时只能逐字 | `lv_ime_phrase_dict.h` 4421 条 |

### 2.1 为什么「瞎组合」这么多（P1.1 的机理）

单字组合路径（`lv_keyboard_build_candidates`，lv_keyboard.c:1671）的行为：

1. **切分**：在当前 pos 用拼音 trie 找出所有能匹配的音节（如 `woyao` 可切
   `wo|yao`、`wo|ya|o` 等），**按匹配长度从长到短**尝试；
2. **展开**：对每个命中的音节，把 `pinyin_dict` 条目的 `chinese` 整串**逐字**
   全部展开递归（lv_keyboard.c:1714-1727 的 `while(*p)` 循环）；
3. **排序**：递归顺序即候选顺序 —— 只体现「切分长度优先」和「字库字串顺序」，
   没有第三种权重。

后果：

- 每个音节的候选就是该拼音**全量字表**（如 `yao` 有 40+ 字全部参与），
  生僻字（镺、䆞…）和常用字（要、也）拥有同等出场机会，只由字表字序决定先后；
- 音节表字序本身不是词频序。实测 `lv_ime_dict.h`：`wo` 前 10 字「我喔窝握沃卧…」
  这个还行，但 `yi` 前 10 是「以一亿已亦易…」（一 反而第 2）、`yao` 里「要」
  排在「腰邀吆…」之后 —— 于是「woyao」的前几屏都是「卧窑哟…」类组合；
- 多音节输入时组合数是各音节字数之积，512 访问预算还会**提前截断**，常用组合
  可能根本没被生成就已经到 100 条上限了。

即：**瞎组合不是 bug，是「无权重全枚举 + 字序即序」这套设计的必然输出**。
修法不是删生僻字，而是给单字路径引入「字频」维度 —— 见方案 C 的 DP 打分，
其中单字 freq 就是缺失的那块数据。

## 3. 改进方案

> P0/P1/P2 都在 LVGL fork（`Cookie987/lvgl`）里改；managed component 会整体
> 重新拉取，**不能改 `.esphome/build/` 里的副本**。宿主侧钩子在
> LemonadeOS（聊天页 lambda 或 `lua_runtime`）里接。

### 方案 A：修数字键翻页 bug（P0，一行，建议最先发）

```c
// lv_keyboard_handle_ime_chn() 中，原来：
//   int absolute_idx = keyboard->candidate_page * LV_KEYBOARD_IME_PAGE_SIZE + local_idx;
// candidate_page 本来就是候选列表的绝对起始下标，直接加即可：
int absolute_idx = keyboard->candidate_page + local_idx;
```

- 风险：无。翻页逻辑 `lv_keyboard_ime_page_step()` 本身就是按起始下标推进的。
- 验证：输入词库命中的词 → 翻到第 2 页 → 按 1-9，应选中屏幕上对应候选。

### 方案 B：用户词频学习（P2）

目标：选过一次的词，之后同样输入时排到前面；重启不丢。

**fork 侧（lv_keyboard.c/.h）**

1. RAM 侧一张小型 LRU 表（建议 64 条上限，条目 `pinyin[16] + utf8 词 + score`）：

```c
typedef struct { char pinyin[16]; char word[19]; uint32_t bonus; } lv_ime_user_entry_t;
// 提交候选时（lv_keyboard_select_candidate 成功路径）：
//   1) 找到本次命中的词库条目（或用户词）→ bonus += 步进，封顶防饱和
//   2) 未命中词库的（拼音, 词）整条插入用户表 → 今后可作为候选输出
// 排序：lv_keyboard_phrase_score() 末尾叠加 user_bonus 查表结果
```

2. 暴露宿主钩子（C 库不用事件，直接回调 + 读取接口）：

```c
/* 提交时回调（pinyin = 本次组合串, word = 上屏词） */
void lv_keyboard_set_ime_commit_cb(lv_keyboard_ime_commit_cb_t cb, void * user_data);
/* 键盘创建后 / 宿主想注入时调用；按 score 高的先注入 */
void lv_keyboard_ime_user_entry_add(const char * pinyin, const char * word, uint32_t score);
```

**宿主侧（LemonadeOS）**

- 落盘介质：`fskv`（NVS，key 数量和值长都合适；用户词表控制在 ~50 条、每条 ≤ 60B，约 3-4KB，NVS 可承受）。**不要放 SD**：聊天页已经处理拔卡回退，输入法是系统级组件不该依赖卡。
- 写入时机：commit 回调里**节流**（例如同词 5 分钟内只写一次 fskv，RAM 表实时生效），避免写磨损。
- 恢复时机：`keyboard_message_show` 脚本第一次拉起键盘时，从 fskv 读出并 `lv_keyboard_ime_user_entry_add()`。
- Lua 应用侧（calendar 等自带键盘的）暂不接钩子，共享同一份 fskv 数据即可（key 统一放 `ime/user_dict`）。

- 成本：fork ~150 行；宿主 ~80 行 YAML lambda。RAM +~2KB。
- 效果：常用词首位化 + 词库外词条可上屏，收益最大、风险最小。

### 方案 C：词组作为组合单元 + 字频打分 → 整句输入、消灭瞎组合（P1 / P1.1）

现状：`lv_keyboard_build_candidates()` 递归时只从 `pinyin_dict`（单字）取字，
所以 woyaochifan 只能产出「我你 吃饭 方…」这类逐字串。改法分两件事，
**缺一不可**（只做前者，逐字候选的瞎组合依旧）：

**C-1 把词表条目也作为切分单元**：

```
递归 build(pinyin, pos, tmp):
    匹配集 M = pinyin_trie(pos) 的精确匹配            # 单字，现有
    词组集 P = phrase_pinyin_trie 从 pos 起的精确匹配  # 新增：
        # phrase_pinyin_trie 是「整词拼音」trie；在 pos 处要求
        # entry.pinyin == pinyin[pos .. pos+len(entry.pinyin)) 完全相等
    取 M ∪ P 中拼音最长者优先展开（保持现有 longest-first 语义）
    展开 = 词组整串 append（或逐字遍历），score 累加该条目 freq
    递归 build(pinyin, pos + len, ...)
```

**C-2 给单字引入字频权重（治瞎组合的根本）**：

1. `lv_ime_dict.h` 加一列 `uint16_t freq`（用和词库同一套 Rime/语料管线
   生成单字字频表，常用字 9999，生僻字 0-9）。430 个音节全表加一列，
   flash 增量 ~1KB，可忽略；
2. 候选展开顺序改为「字频降序」：`chinese` 串顺序不再直接决定候选序，
   每个音节先按 freq 排好（可在建 trie 时预排序索引，运行时零开销）；
3. **生僻字截断**：`freq == 0` 的字默认不参与候选（或仅在长按/末页出现），
   一下把「卧窑哟」这类组合清出候选条 —— 每个音节实际可用字收敛到 10-20 个；
4. 512 访问预算因为候选变少而绰绰有余，常用组合不会再被截断丢失。

**C-3 排序：DP 切分打分**（纯 DFS 顺序对多组合输入几乎无效，必须带打分剪枝）：

- 先用 DP 求最优切分：`dp[pos] = max over (单字 freq, 词组 3e9+freq) + dp[next]`，
  即「整词优先、词组内部不再拆」；DP 状态 ≤ 17 个位置，代价可忽略；
- 候选生成按「DP 最优切分路径 > 其余切分」排序，每条路径内按 C-2 的字频序输出。
  这样 woyaochifan 直接给出「我要吃饭 / 我要吃…」，而 woyao 首候选就是「我要」，
  后备是「我压 / 卧遥」等高频字组合而非「卧窑哟」。

要点与代价：

1. 与现有 512 访问预算兼容：DP 与打分求解不计入预算，候选生成仍限 100 条；
2. 现有「词组精确匹配优先」路径保持不变（短输入行为不变），C 只改
   `lv_keyboard_collect_char_candidates` 那条逐字路径；
3. 风险：`lv_ime_phrase_dict.h` 与 `pinyin_dict` 的切分可能重叠（如词库
   「时候」 vs 单字组合），用 DP 分数天然消歧（词组 3e9 基数 > 任何单字串）；
4. C-2 的字频表和方案 D 的词库生成可以共用一条 Rime 管线，建议合并实施；
5. 成本：fork 内 ~300 行 + 字频数据生成 + 充分手测。

### 方案 D：扩词库（P3）

1. **首选：运行时从 SD/littlefs 加载进 PSRAM**，不再编进固件：
   - 生成步骤沿用现有 Rime 管线（头文件注释「Auto-generated from Rime dict」），
     输出改成紧凑二进制（拼音 → 音节 id 序列、词 → utf8 + uint32 freq），
     目标 5-10 万条 ≈ 2-4MB；
   - `lv_keyboard` 加 `lv_keyboard_ime_load_dict(buf, size)`（替换静态表），
     trie 建树挪到首次进入中文模式时；
   - 放置路径：littlefs `/data` 由 OTA 布局保证存在，2MB 分区够 4MB 的话
     压缩存 SD 并缓存 PSRAM（PSRAM 8MB，字库+词库常驻 ~5MB 可接受，但要注意
     Lua 应用「PSRAM 低于 1MB 才停止」的预算约定）；
   - app 分区不动 → 不受 6.8MB 双 OTA 限制，这是它能大幅扩量的前提。
2. **保守替代**：仍编进固件，但只加 2 字高频词至 ~2 万条（flash +~700KB），
   需先核对 `partitions_16M.csv` 余量，风险较高，不推荐先做。

## 4. 实施顺序建议

```
A（修 bug，1 天）→ B（词频学习，2-3 天）→ C-2（字频表 + 生僻字截断，
直接治瞎组合，可与 D 的数据管线合并）→ D（SD 词库，独立进行）
→ C-1/C-3（整句 DP，最大改动，最后做，且要在 D 的新词库规模上验证性能）
```

每个 fork 改动都要同步回归 T / T_3.2 两个机型（`esphome config` +
`compile`）；B 的 fskv key、D 的文件路径要写进 changelog。

## 5. 待确认问题

1. B 的用户词容量上限（fskv NVS 预算）；
2. D 走 littlefs 还是 SD（决定词库量级和 Lua 应用可用空间）；
3. C-2 字频表用哪份语料生成（Rime 主词表 vs 固有语料），以及 `freq == 0`
   生僻字是「完全不出现」还是「排末页」；
4. C 是否要支持「逐字 + 词组混合上屏后继续组句」（即空格提交第一个候选后，
   后续拼音基于前词上下文续推 —— 这是第二步，当前方案不含）。
