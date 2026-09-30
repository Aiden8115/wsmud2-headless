// 游戏 HTML 颜色标记 → ANSI 转义序列（终端染色显示）
// 处理 <NOR>/<GRE>/<HIR>… 等 23 种颜色标签、<opt>（半透明→暗色）、class="hide"（隐藏丢弃），
// 以及 &nbsp; &amp; &#39; &#NNN; 等 HTML 实体；未识别的结构化标签剔除、未知标签原样保留。
#pragma once

#include <string>

namespace wsmud {

// 把一段游戏下发的 HTML 文本转成带 ANSI 颜色的纯文本（可安全直接进日志/终端）。
std::string html_to_ansi(const std::string& html);

}  // namespace wsmud