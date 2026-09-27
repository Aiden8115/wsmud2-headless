# wsmud2_headless — 触发器配置（trigger.json）

触发器不再通过界面编辑，一律通过软件同级目录下的 **`trigger.json`** 维护。
程序对该文件**只读**，唯一的写操作是首次启动时若文件不存在则创建空模板 `{"version":1,"triggers":[]}`。

- 修改文件后，在任意位置输入命令 **`reloadTrigger`** 立即重载（成功/失败都有提示，失败保留旧配置）。
- 重载成功后，**所有已登录账号**立即生效；后续新增标签页自动继承同一份配置。
- F8 打开当前账号的只读触发器列表，`trigger list` 命令查看同一份列表。

---

## 1. JSON 语法树（总览）

```text
trigger.json（顶层 JSON 对象）
│
├── version：整数 ——（必填）固定为 1
│
└── triggers：数组 [] —— 触发器列表，可为空
    │
    └── （数组项）触发器对象 {}，每项含以下字段：
        │
        ├── name：字符串 —— 触发器名（中文/英文/数字/下划线），同一文件内不可重复
        │
        ├── event：字符串 —— 事件类型，15 种枚举之一；取值决定下方 conds 允许的键集合
        │
        ├── active：布尔 —— true=启用 / false=停用
        │
        ├── conds：对象 {} —— 触发条件；对象内部结构随上方 event 取值而变：
        │   │
        │   │当 event = hint / social （新提示信息/社交消息）时：
        │   ├── keyword：字符串 —— 任意文本，包含匹配
        │   │
        │   │当 event = auction （拍卖查询）时：
        │   ├── keyword：字符串 —— 任意文本，包含匹配
        │   ├── item_level：整数 —— 颜色等级：0白|1绿|2蓝|3黄|4紫|5橙
        │   │
        │   │当 event = activity （活动事件）时：
        │   ├── name：字符串 —— 活动名称，包含匹配
        │   ├── keyword：字符串 —— 任意文本，包含匹配
        │   │
        │   │当 event = chat （新聊天信息）时：
        │   ├── channel：字符串 —— 取值：全部|世界|队伍|门派|全区|帮派|谣言|系统
        │   ├── speaker：字符串 —— 发言人，包含匹配
        │   ├── ignore_speaker：字符串 —— 忽略的发言人（不包含匹配）
        │   ├── keyword：字符串 —— 任意文本，包含匹配
        │   │
        │   │当 event = char_refresh （人物刷新）时：
        │   ├── char_name：字符串 —— 人物名称，包含匹配
        │   │
        │   │当 event = item_pickup （物品拾取）时：
        │   ├── name_keyword：字符串 —— 物品名称，包含匹配
        │   │
        │   │当 event = buff_change （Buff状态改变）时：
        │   ├── change_type：字符串 —— 取值：新增|移除|层数刷新
        │   ├── buff_id：字符串 —— Buff ID 名称（如 busy/faint），支持 || 或
        │   ├── target：字符串 —— 取值：自己|他人
        │   │
        │   │当 event = combat / death （战斗状态切换/死亡状态改变）时：
        │   ├── type：字符串 —— combat 取值：进入战斗|脱离战斗；death 取值：已经死亡|已经复活
        │   │
        │   │当 event = time_reached （时辰已到）时：
        │   ├── hour：整数 —— 时（0-23）
        │   ├── minute：整数 —— 分（0-59）
        │   ├── second：整数 —— 秒（0-59）
        │   │
        │   │当 event = skill_cast / skill_cd （技能释放/技能冷却结束）时：
        │   ├── skill_id：字符串 —— 技能名称，支持 || 或
        │   │
        │   │当 event = hp_mp （气血内力改变）时：
        │   ├── name_keyword：字符串 —— 人物名称，包含匹配
        │   ├── type：字符串 —— 取值：气血|内力
        │   ├── when：字符串 —— 取值：低于|高于
        │   ├── value_type：字符串 —— 取值：百分比|数值
        │   ├── value：字符串 —— 阈值，写数字（整数/小数；百分比模式即百分比数）
        │   │
        │   │当 event = damage_full （伤害已满）时：
        │   ├── name_keyword：字符串 —— 人物名称，包含匹配
        │   ├── value_type：字符串 —— 取值：百分比|数值
        │   └── value：字符串 —— 伤害阈值，写数字（整数/小数）
        │   各条件键的值：空 或 * = 通配（不设限）；字符串值用 || 分隔多个候选（或匹配）；整数键写整数
        │
        └── source：数组 [] / 字符串 —— 脚本指令，一行一条（与 event 无关，所有事件通用）
            │   （旧版字符串格式按换行拆分；新版推荐数组格式）
            │
            └── （字符串，表达一行指令。）
```

---

## 2. 字段说明

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `version` | 整数 | 是 | 固定为 `1` |
| `triggers` | 数组 | 是 | 触发器列表，可为空数组 |
| `name` | 字符串 | 是 | 触发器名（中文/英文/数字/下划线）；同名会校验失败 |
| `event` | 字符串 | 是 | 事件类型，见 §3 的 15 种枚举，须完整匹配 |
| `active` | 布尔 | 是 | `true` 启用 / `false` 停用 |
| `conds` | 对象 | 是 | 条件对象，可为空对象 `{}`；**键名**必须属于该事件模板；hour/minute/second/item_level 写整数，其余键写字符串，取值枚举见 §1 |
| `source` | 数组 / 字符串 | 是 | 脚本指令；数组为一行一条，字符串按换行拆分（旧格式兼容） |

> 条件值为空或 `*` 表示不设限制；字符串值用 `||` 分隔多个候选表示"或"（如 `"speaker": "张三||李四"`）。

---

## 3. 事件类型（event）与条件键（conds）

| 事件类型（event） | 可用条件键（conds，断言语义） |
|-------------------|-------------------------------|
| `hint`（新提示信息） | `keyword`（keyword） |
| `social`（社交消息） | `keyword`（keyword） |
| `auction`（拍卖查询） | `keyword`（keyword）、`item_level`（contains，整数 0-5） |
| `activity`（活动事件） | `name`（activity_name）、`keyword`（keyword） |
| `chat`（新聊天信息） | `channel`（channel）、`speaker`（contains）、`ignore_speaker`（not_contains）、`keyword`（keyword） |
| `char_refresh`（人物刷新） | `char_name`（keyword） |
| `item_pickup`（物品拾取） | `name_keyword`（keyword） |
| `buff_change`（Buff状态改变） | `change_type`（equal）、`buff_id`（contains，字符串）、`target`（equal） |
| `combat`（战斗状态切换） | `type`（equal） |
| `death`（死亡状态改变） | `type`（equal） |
| `time_reached`（时辰已到） | `hour` / `minute` / `second`（time_reached，整数） |
| `skill_cast`（技能释放） | `skill_id`（contains，字符串） |
| `skill_cd`（技能冷却结束） | `skill_id`（contains，字符串） |
| `hp_mp`（气血内力改变） | `name_keyword`（keyword）、`type`（equal）、`when`（equal）、`value_type`（equal）、`value`（crossing） |
| `damage_full`（伤害已满） | `name_keyword`（keyword）、`value_type`（equal）、`value`（crossing_up） |

---

## 4. source 脚本指令（@指令）

| 指令 | 作用 |
|------|------|
| `look`、`go north`（普通行） | 原样作为游戏命令发送 |
| `@wait <毫秒>` | 延时指定毫秒后执行下一条 |
| `@tip <正则>` | 等待游戏输出匹配该正则后继续 |
| `@until <表达式>` | 等待条件表达式成立 |
| `@kill <流程名>` | 停止指定名称的流程 |
| `@exit` / `@stop` | 退出当前循环 / 停止当前流程 |
| `@next` | 继续下一流程 |
| `@print` / `@show <文字>` | 在日志提示一段文字 |
| `@debug <文字>` | 输出调试日志 |
| `@force <命令>` | 强制发送游戏命令（支持占位符） |
| `@perform <技能1>,<技能2>` | 依次执行 perform |
| `@cd <技能名>` | 等待技能冷却结束 |
| `@cmdDelay <毫秒>` | 设置命令发送间隔 |
| `@cleanBag` / `@tidyBag` | 卖空背包 / 卖空并存库 |
| `@renew` | 重新开始当前流程 |
| `@liaoshang` / `@dazuo` | 等待疗伤 / 打坐状态 |
| `@on` / `@off <触发器名>` | 动态启用 / 停用触发器 |
| `@js <表达式>` | 执行一段 JS 表达式 |
| `#...` 开头 | 无界面占位指令，跳过 |

---

## 5. 完整示例

```json
{
  "version": 1,
  "triggers": [
    {
      "name": "战斗结束",
      "event": "combat",
      "active": true,
      "conds": { "type": "脱离战斗" },
      "source": ["look", "@tip 战斗结束"]
    },
    {
      "name": "自动疗伤",
      "event": "hp_mp",
      "active": true,
      "conds": {
        "type": "气血",
        "when": "低于",
        "value_type": "百分比",
        "value": "50"
      },
      "source": ["@wait 2000", "@perform heal"]
    },
    {
      "name": "子时提醒",
      "event": "time_reached",
      "active": true,
      "conds": { "hour": 23 },
      "source": ["@tip 子时已到"]
    },
    {
      "name": "监听某人说话",
      "event": "chat",
      "active": false,
      "conds": { "speaker": "张三", "channel": "世界" },
      "source": ["@tip 张三说话了"]
    }
  ]
}
```

---

## 6. 常见校验错误

| 现象 | 原因 |
|------|------|
| `事件类型未知：xxx` | `event` 不在 §3 的 15 种枚举中 |
| `条件「xxx」不属于事件「yyy」` | `conds` 的键名不是该事件过滤器中定义的条件键 |
| `名称重复：xxx` | 两个触发器同名 |
| `触发器缺少事件类型` | 缺少 `event` 字段 |
| 重载失败保留旧配置 | 文件不是合法 JSON（可用 `jq . trigger.json` 自查） |
