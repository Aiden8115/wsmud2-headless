// ============================================================
// raid_interp.js —— 精简 Raid 脚本解释器（运行在 QuickJS 中）
// ------------------------------------------------------------
// 由 C++ 宿主注入 __cxx 对象（见 trigger.cpp）：
//   __cxx.log(text)                输出到账号日志
//   __cxx.send(cmd)                发送游戏命令
//   __cxx.state()                  返回 JSON 字符串 {hp,maxHp,mp,maxMp,state,room,
//                                    living,combat,free,idle,idle_time,level,name}
//   __cxx.roomItemId(name,exact)   房间缓存里按名称找人物 id（精确/模糊），返回 id 或 null
//   __cxx.roomHasItem(name,exact)  房间缓存里是否还有该人物
//   __cxx.coolingUntil(skillId)    技能冷却结束时间戳（毫秒），无信息返回 0
//   __cxx.setTrigger(name,active)  启用/停用触发器（@on/@off）
//   __cxx.now()                    当前毫秒时间戳
//
// 对外接口（由 C++ 调用）：
//   __start(name, source)          启动一段流程（同名未运行才允许）
//   __tick()                        推进所有流程（C++ 主循环每帧调用）
//   __feed_text(text)              把新提示文本喂给等待 @tip 的流程
// ============================================================
'use strict';

var __flows = [];
var _st = null;          // 本帧状态缓存
var _curFlow = null;     // 当前执行流程（表达式求值需要访问其变量）

// ---------- 工具 ----------

function stripTags(s) {
    return String(s).replace(/<[^>]*>/g, '');
}
function truthy(v) {
    if (v == null) return false;
    return Boolean(v);
}
function getState() {
    if (_st == null) refreshState();
    return _st;
}
function refreshState() {
    try {
        _st = JSON.parse(__cxx.state());
    } catch (e) {
        _st = {};
    }
    if (_st == null) _st = {};
    var raw = _st.state == null ? '' : String(_st.state);
    _st.stateRaw = raw;
    _st.stateNorm = normState(raw);
}
function normState(raw) {
    if (!raw) return 'none';
    if (raw.indexOf('打坐') != -1) return 'dazuo';
    if (raw.indexOf('疗伤') != -1) return 'liaoshang';
    if (raw.indexOf('战斗') != -1) return 'fight';
    if (raw.indexOf('发呆') != -1 || raw.indexOf('空闲') != -1) return 'idle';
    return 'none';
}

// ---------- 表达式求值 ----------

function tokenize(s) {
    var toks = [], i = 0, n = s.length;
    while (i < n) {
        var c = s[i];
        if (/\s/.test(c)) { i++; continue; }
        if (c == '(') {
            var depth = 1, j = i + 1, inner = '';
            while (j < n && depth > 0) {
                var d = s[j];
                if (d == '(') depth++;
                else if (d == ')') depth--;
                if (depth > 0) inner += d;
                j++;
            }
            toks.push({ t: 'paren', inner: inner });
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {
            var q = c, j2 = i + 1, str = '';
            while (j2 < n && s[j2] != q) { str += s[j2]; j2++; }
            toks.push({ t: 'str', v: str });
            i = j2 + 1;
            continue;
        }
        if (/[0-9]/.test(c)) {
            var j3 = i;
            while (j3 < n && /[0-9.]/.test(s[j3])) j3++;
            toks.push({ t: 'num', v: parseFloat(s.slice(i, j3)) });
            i = j3;
            continue;
        }
        if (c == '=' && s[i + 1] == '=') { toks.push({ t: 'op', v: '==' }); i += 2; continue; }
        if (c == '!' && s[i + 1] == '=') { toks.push({ t: 'op', v: '!=' }); i += 2; continue; }
        if (c == '<') { if (s[i + 1] == '=') { toks.push({ t: 'op', v: '<=' }); i += 2; } else { toks.push({ t: 'op', v: '<' }); i++; } continue; }
        if (c == '>') { if (s[i + 1] == '=') { toks.push({ t: 'op', v: '>=' }); i += 2; } else { toks.push({ t: 'op', v: '>' }); i++; } continue; }
        if (c == '&' && s[i + 1] == '&') { toks.push({ t: 'op', v: '&&' }); i += 2; continue; }
        if (c == '|' && s[i + 1] == '|') { toks.push({ t: 'op', v: '||' }); i += 2; continue; }
        if (c == '!') { toks.push({ t: 'op', v: '!' }); i++; continue; }
        var j4 = i;
        while (j4 < n && !/[\s()=!<>&|]/.test(s[j4])) j4++;
        toks.push({ t: 'word', v: s.slice(i, j4) });
        i = j4;
    }
    return toks;
}

function presetRaw(name) {
    var st = getState();
    switch (name) {
        case 'hp': return st.hp == null ? 0 : st.hp;
        case 'maxHp': return st.maxHp == null ? 0 : st.maxHp;
        case 'mp': return st.mp == null ? 0 : st.mp;
        case 'maxMp': return st.maxMp == null ? 0 : st.maxMp;
        case 'hpPer': return (st.maxHp ? st.hp / st.maxHp : 0);
        case 'mpPer': return (st.maxMp ? st.mp / st.maxMp : 0);
        case 'free': return !!st.free;
        case 'idle': return !!st.idle;
        case 'idle_time': return st.idle_time == null ? 0 : st.idle_time;
        case 'living': return !!st.living;
        case 'combat': return !!st.combat;
        case 'room': return st.room == null ? '' : st.room;
        case 'level': return st.level == null ? 0 : st.level;
        case 'name': return st.name == null ? '' : st.name;
    }
    return null;
}

function presetValue(name, arg) {
    if (name == 'state') {
        if (arg !== undefined) return String(getState().stateRaw).indexOf(String(arg)) != -1;
        return getState().stateNorm;
    }
    var base = presetRaw(name);
    if (arg !== undefined) return String(base == null ? '' : base).indexOf(String(arg)) != -1;
    return base;
}

function parseParenInner(inner, flow) {
    inner = String(inner).trim();
    if (!inner) return '';
    if (/^:[A-Za-z0-9_\u4e00-\u9fa5]+(\s+[\s\S]*)?$/.test(inner)) {
        var sp = inner.indexOf(' ');
        var name = sp < 0 ? inner.slice(1) : inner.slice(1, sp);
        var arg = sp < 0 ? undefined : inner.slice(sp + 1).trim();
        return presetValue(name, arg);
    }
    if (/^\$[A-Za-z0-9_]+$/.test(inner)) {
        return flow.vars[inner.slice(1)];
    }
    if (/[|&<>!=]/.test(inner)) {
        return evalExpr(inner, flow);
    }
    if (inner == 'true') return true;
    if (inner == 'false') return false;
    if (inner == 'null' || inner == 'undefined') return null;
    if (/^-?[0-9.]+$/.test(inner)) return parseFloat(inner);
    if (flow.vars.hasOwnProperty(inner)) return flow.vars[inner];
    return inner;
}

function looseEq(a, b) {
    if (typeof a == typeof b) return a === b;
    if ((a == null) || (b == null)) return a === b;
    if (typeof a == 'number' && typeof b == 'string') return a === parseFloat(b);
    if (typeof a == 'string' && typeof b == 'number') return parseFloat(a) === b;
    return String(a) === String(b);
}
function cmp(l, o, r) {
    if (o == '==') return looseEq(l, r);
    if (o == '!=') return !looseEq(l, r);
    var ln = Number(l), rn = Number(r);
    if (o == '<') return ln < rn;
    if (o == '>') return ln > rn;
    if (o == '<=') return ln <= rn;
    if (o == '>=') return ln >= rn;
    return false;
}

function evalExpr(s, flow) {
    try {
        var toks = tokenize(String(s == null ? '' : s));
        var pos = 0;
        function peek() { return toks[pos]; }
        function next() { return toks[pos++]; }
        function parseOr() {
            var v = parseAnd();
            while (peek() && peek().t == 'op' && peek().v == '||') { next(); var r = parseAnd(); v = truthy(v) || truthy(r); }
            return v;
        }
        function parseAnd() {
            var v = parseNot();
            while (peek() && peek().t == 'op' && peek().v == '&&') { next(); var r = parseNot(); v = truthy(v) && truthy(r); }
            return v;
        }
        function parseNot() {
            if (peek() && peek().t == 'op' && peek().v == '!') { next(); return !truthy(parseNot()); }
            return parseCmp();
        }
        function parseCmp() {
            var l = parseUnary();
            if (peek() && peek().t == 'op' && /^(==|!=|<|>|<=|>=)$/.test(peek().v)) {
                var o = next().v;
                var r = parseUnary();
                return cmp(l, o, r);
            }
            return l;
        }
        function parseUnary() {
            if (peek() && peek().t == 'op' && peek().v == '-') { next(); return -Number(parseUnary()); }
            return parsePrimary();
        }
        function parsePrimary() {
            var t = peek();
            if (!t) return null;
            next();
            if (t.t == 'num') return t.v;
            if (t.t == 'str') return t.v;
            if (t.t == 'word') {
                var w = t.v;
                if (w == 'true') return true;
                if (w == 'false') return false;
                if (w == 'null' || w == 'undefined') return null;
                return w;
            }
            if (t.t == 'paren') return parseParenInner(t.inner, flow);
            return null;
        }
        return parseOr();
    } catch (e) {
        return String(s == null ? '' : s).trim();
    }
}

// ---------- 编译：源码 → 指令树 ----------

function preprocess(source) {
    var s = String(source).replace(/\/\*[\s\S]*?\*\//g, '');
    var raw = s.split('\n');
    var lines = [];
    var inRegion = false;
    for (var k = 0; k < raw.length; k++) {
        var line = raw[k].replace(/\r$/, '');
        var trimmed = line.trim();
        if (!trimmed) continue;
        if (/^<===/.test(trimmed) || /^<---/.test(trimmed)) { inRegion = true; continue; }
        if (inRegion) {
            if (trimmed.indexOf('===>') != -1 || trimmed.indexOf('--->') != -1) inRegion = false;
            continue;
        }
        if (trimmed.indexOf('//') == 0) continue;
        var ind = /^ */.exec(line)[0].length;
        lines.push({ ind: ind, text: trimmed });
    }
    return lines;
}

function parseBody(lines, i, controlIndent) {
    var ops = [];
    var elseOps = null;
    var cur = ops;
    while (i < lines.length) {
        var ln = lines[i];
        if (ln.ind <= controlIndent) {
            var m = /^\[(else|elseif)\]\s*(.*)$/.exec(ln.text);
            if (m && ln.ind == controlIndent) {
                if (m[1] == 'else') { i++; elseOps = []; cur = elseOps; continue; }
                var cond = m[2], i2 = i + 1;
                var sub = parseBody(lines, i2, controlIndent);
                if (elseOps == null) elseOps = [];
                elseOps.push({ t: 'if', cond: cond, then: sub.ops, else: sub.elseOps });
                cur = elseOps;
                i = sub.next;
                continue;
            }
            break;
        }
        var r = parseStatement(lines, i);
        for (var q = 0; q < r.ops.length; q++) cur.push(r.ops[q]);
        i = r.next;
    }
    return { ops: ops, elseOps: elseOps, next: i };
}

function parseStatement(lines, i) {
    var ln = lines[i];
    var text = ln.text;
    var ind = ln.ind;
    if (text[0] == '[') {
        var m = /^\[(if|while|elseif|else|exit|break|continue)\]\s*(.*)$/.exec(text);
        if (m && (m[1] == 'if' || m[1] == 'while')) {
            var kw = m[1], cond = m[2];
            var body = parseBody(lines, i + 1, ind);
            return { ops: [{ t: kw, cond: cond, then: body.ops, else: body.elseOps }], next: body.next };
        }
        if (m && m[1] == 'exit') return { ops: [{ t: 'exit' }], next: i + 1 };
        if (m && m[1] == 'break') return { ops: [{ t: 'break' }], next: i + 1 };
        if (m && m[1] == 'continue') return { ops: [{ t: 'continue' }], next: i + 1 };
        if (m) return { ops: [], next: i + 1 };   // 悬挂的 else/elseif，跳过
        if (text.indexOf('[=') == 0) {
            var close = text.indexOf(']');
            if (close > 1) {
                var cond2 = text.substring(2, close);
                var cmd = text.substring(close + 1).trim();
                if (cmd) return { ops: [{ t: 'until', expr: cond2 }, { t: 'cmd', text: cmd }], next: i + 1 };
                return { ops: [{ t: 'until', expr: cond2 }], next: i + 1 };
            }
        }
        return { ops: [{ t: 'cmd', text: text }], next: i + 1 };
    }
    var am = /^\(\$([A-Za-z0-9_]+)\)\s*=\s*([\s\S]+)$/.exec(text);
    if (am) return { ops: [{ t: 'assign', name: am[1], expr: am[2] }], next: i + 1 };
    return { ops: [{ t: 'cmd', text: text }], next: i + 1 };
}

function compile(source) {
    var lines = preprocess(source);
    var body = parseBody(lines, 0, -1);
    return body.ops;
}

// ---------- 流程运行时 ----------

function makeFlow(name, source) {
    var ops = compile(source);
    return {
        name: name,
        ops: ops,
        vars: {},
        wait: null,
        finish: false,
        cmdDelay: 1500,
        lastCmdMs: -999999,
        sendQueue: [],
        opsSinceWait: 0,
        steps: 0,
        startMs: __cxx.now()
    };
}

function startFlow(name, source) {
    for (var k = 0; k < __flows.length; k++) {
        if (__flows[k].name == name) {
            __cxx.log('[触发] "' + name + '" 已在运行，本次触发忽略');
            return;
        }
    }
    var flow = makeFlow(name, source);
    flow.stack = [{ kind: 'root', ops: flow.ops, pc: 0 }];
    __flows.push(flow);
}

function evalCond(expr, flow) {
    return truthy(evalExpr(expr, flow));
}

function pushFrame(flow, frame) { flow.stack.push(frame); }

function execOp(flow, op) {
    flow.steps++;
    flow.opsSinceWait++;
    switch (op.t) {
        case 'cmd':
            execCmd(flow, op.text);
            break;
        case 'assign':
            flow.vars[op.name] = evalExpr(op.expr, flow);
            break;
        case 'until':
            flow.wait = { t: 'cond', expr: op.expr };
            break;
        case 'exit':
            flow.finish = true;
            break;
        case 'break':
            doBreak(flow);
            break;
        case 'continue':
            doContinue(flow);
            break;
        case 'if':
            if (evalCond(op.cond, flow)) pushFrame(flow, { kind: 'block', ops: op.then, pc: 0 });
            else if (op.else && op.else.length) pushFrame(flow, { kind: 'block', ops: op.else, pc: 0 });
            break;
        case 'while':
            if (evalCond(op.cond, flow)) pushFrame(flow, { kind: 'whileBody', ops: op.then, pc: 0, whileOp: op });
            break;
    }
}

function doBreak(flow) {
    while (flow.stack.length) {
        var f = flow.stack[flow.stack.length - 1];
        if (f.kind == 'whileBody' || f.kind == 'while') { flow.stack.pop(); return; }
        flow.stack.pop();
    }
    flow.finish = true;
}
function doContinue(flow) {
    while (flow.stack.length) {
        var f = flow.stack[flow.stack.length - 1];
        if (f.kind == 'whileBody') {
            flow.stack.pop();
            if (evalCond(f.whileOp.cond, flow)) pushFrame(flow, { kind: 'whileBody', ops: f.whileOp.then, pc: 0, whileOp: f.whileOp });
            return;
        }
        flow.stack.pop();
    }
    flow.finish = true;
}

function popFrame(flow) {
    var f = flow.stack.pop();
    if (!f) return;
    if (f.kind == 'whileBody') {
        // 循环体结束：重新检查条件（可能有别的帧导致顺序复杂，但块内 break/continue 已处理）
        if (evalCond(f.whileOp.cond, flow)) {
            pushFrame(flow, { kind: 'whileBody', ops: f.whileOp.then, pc: 0, whileOp: f.whileOp });
        }
    }
}

// 每帧推进一个流程；返回 true 表示该流程已结束
function tickFlow(flow) {
    var steps = 0;
    for (;;) {
        if (flow.finish) return true;
        if (flow.wait) {
            if (waitDone(flow)) { flow.wait = null; flow.opsSinceWait = 0; continue; }
            if (flow.opsSinceWait > 30000) {
                __cxx.log('[流程] "' + flow.name + '" 长时间无进度，已强制停止');
                return true;
            }
            return false;
        }
        if (++steps > 300) return false;
        var f = flow.stack[flow.stack.length - 1];
        if (!f) return true;
        if (f.pc >= f.ops.length) { popFrame(flow); continue; }
        var op = f.ops[f.pc++];
        execOp(flow, op);
    }
}

function waitDone(flow) {
    var w = flow.wait;
    var now = __cxx.now();
    switch (w.t) {
        case 'ms':
        case 'cmdDelay':
            return now >= w.until;
        case 'cond':
            return evalCond(w.expr, flow);
        case 'tip':
            return !!w.hit;
        case 'sendQueue':
            return sendQueueDone(flow, now);
        case 'kill':
            return killDone(flow, w, now);
        case 'state':
            if (getState().stateNorm == w.want) return true;
            if (now >= w.deadline) { __cxx.log('[提示] 等待状态 "' + w.want + '" 超时，继续'); return true; }
            return false;
        case 'renew':
            return renewDone(flow, w, now);
        case 'cd':
            if (now >= w.deadline) { __cxx.log('[提示] @cd 等待超时，继续'); return true; }
            for (var k = 0; k < w.skills.length; k++) {
                var u = __cxx.coolingUntil(w.skills[k]);
                if (u && u > now) return false;
            }
            return true;
    }
    return true;
}

function sendQueueDone(flow, now) {
    while (flow.sendQueue.length && flow.sendQueue[0].at <= now) {
        var it = flow.sendQueue.shift();
        __cxx.send(it.cmd);
        flow.lastCmdMs = now;
    }
    return flow.sendQueue.length == 0;
}

function killDone(flow, w, now) {
    if (now >= w.deadline) { __cxx.log('[失败] @kill 超时，停止流程'); flow.finish = true; return true; }
    var allDone = true;
    for (var k = 0; k < w.targets.length; k++) {
        var tg = w.targets[k];
        if (__cxx.roomHasItem(tg.name, tg.exact)) { allDone = false; break; }
    }
    if (allDone) return true;
    if (now - w.lastTry >= 1500) {
        w.lastTry = now;
        for (var k2 = 0; k2 < w.targets.length; k2++) {
            var tg2 = w.targets[k2];
            if (__cxx.roomHasItem(tg2.name, tg2.exact)) {
                var id = __cxx.roomItemId(tg2.name, tg2.exact);
                if (id) __cxx.send('kill ' + id + ';');
            }
        }
    }
    return false;
}

function renewDone(flow, w, now) {
    switch (w.step) {
        case 0:
            if (now - w.t0 >= 300) { __cxx.send('stopstate;liaoshang'); w.step = 1; w.t0 = now; }
            return false;
        case 1:
            if (getState().stateNorm == 'liaoshang') { w.step = 2; return false; }
            if (now - w.t0 >= 120000) { __cxx.log('[提示] 疗伤等待超时，继续'); w.step = 2; return false; }
            return false;
        case 2: {
            var st = getState();
            if (st.mpPer != null && st.mpPer < 0.8) __cxx.send('stopstate;dazuo');
            return true;
        }
    }
    return true;
}

// ---------- 命令执行 ----------

function resolvePlaceholders(text) {
    return String(text).replace(/\{([^}]+)\}/g, function (m, inner) { return inner.trim(); });
}

function doKill(flow, args) {
    var parts = String(args).split(',').map(function (p) { return p.trim(); }).filter(Boolean);
    var targets = [];
    for (var k = 0; k < parts.length; k++) {
        var p = parts[k];
        var exact = false;
        if (p.charAt(p.length - 1) == '%') { exact = true; p = p.slice(0, -1); }
        targets.push({ name: p, exact: exact });
    }
    if (!targets.length) return;
    flow.wait = { t: 'kill', targets: targets, lastTry: 0, deadline: __cxx.now() + 120000 };
}

function doStateWait(flow, want) {
    if (getState().stateNorm == want) return;
    flow.wait = { t: 'state', want: want, deadline: __cxx.now() + 120000 };
}

function doRenew(flow) {
    __cxx.log('恢复气血内力（简化流程：不回武庙，直接疗伤/打坐）');
    __cxx.send('stopstate');
    flow.wait = { t: 'renew', step: 0, t0: __cxx.now() };
}

function doCd(flow, args) {
    var a = String(args).trim();
    var skills = [];
    if (a.charAt(0) == '^') a = a.slice(1);
    skills = a.split(',').map(function (t) { return t.trim(); }).filter(Boolean);
    flow.wait = { t: 'cd', skills: skills, deadline: __cxx.now() + 60000 };
}

function doJs(flow, args) {
    var m = /^\(\$([A-Za-z0-9_]+)\)\s*=\s*([\s\S]+)$/.exec(String(args));
    if (m) { flow.vars[m[1]] = evalExpr(m[2], flow); return; }
    try { eval(String(args)); }
    catch (e) { __cxx.log('[js错误] ' + e); }
}

function sysCmd(flow, text) {
    var cmd = text, rep = 1;
    var m = /^(.*?)\[(\d+)\]$/.exec(text);
    if (m) { cmd = m[1]; rep = parseInt(m[2], 10) || 1; }
    cmd = resolvePlaceholders(cmd).trim();
    if (!cmd) return;
    var now = __cxx.now();
    var at = flow.lastCmdMs + flow.cmdDelay;
    if (at <= now) at = now + flow.cmdDelay;
    for (var k = 0; k < rep; k++) flow.sendQueue.push({ cmd: cmd, at: at + k * flow.cmdDelay });
    flow.wait = { t: 'sendQueue' };
}

function execCmd(flow, text) {
    text = String(text).trim();
    if (!text) return;
    if (text.charAt(0) == '#') return;                 // #input/#select/#config：无 UI，跳过
    if (text.charAt(0) == '$') {                        // 旧别名
        if (/^\$wait\b/.test(text)) return execCmd(flow, '@wait' + text.slice(5));
        __cxx.log('[提示] 不支持的旧命令：' + text + '，已跳过');
        return;
    }
    if (text == '@next') { doContinue(flow); return; }
    if (text == '@exit') { doBreak(flow); return; }
    if (text.charAt(0) != '@') { sysCmd(flow, text); return; }

    var sp = text.indexOf(' ');
    var name = sp < 0 ? text.slice(1) : text.slice(1, sp);
    var args = sp < 0 ? '' : text.slice(sp + 1).trim();
    var now = __cxx.now();

    switch (name) {
        case 'print':
        case 'show':
            __cxx.log(stripTags(args));
            return;
        case 'debug':
            __cxx.log('[调试] ' + stripTags(args));
            return;
        case 'wait':
        case 'await': {
            var n = parseInt(args, 10) || 0;
            flow.wait = { t: 'ms', until: now + n };
            return;
        }
        case 'until':
            flow.wait = { t: 'cond', expr: args };
            return;
        case 'tip':
            try { flow.wait = { t: 'tip', regex: new RegExp(args), hit: false }; }
            catch (e) { flow.wait = null; }
            return;
        case 'kill':
            doKill(flow, resolvePlaceholders(args));
            return;
        case 'perform':
            args.split(',').map(function (s) { return s.trim(); }).filter(Boolean)
                .forEach(function (s) { __cxx.send('perform ' + s); });
            return;
        case 'renew':
            doRenew(flow);
            return;
        case 'liaoshang':
            doStateWait(flow, 'liaoshang');
            return;
        case 'dazuo':
            doStateWait(flow, 'dazuo');
            return;
        case 'cleanBag':
            __cxx.send('sell all');
            return;
        case 'tidyBag':
            __cxx.send('sell all;store all');
            return;
        case 'stop':
            flow.finish = true;
            return;
        case 'force':
            __cxx.send(resolvePlaceholders(args));
            return;
        case 'cmdDelay': {
            var d = parseInt(args, 10);
            if (d >= 0) flow.cmdDelay = d;
            return;
        }
        case 'cd':
            doCd(flow, args);
            return;
        case 'on':
        case 'off':
            __cxx.setTrigger(args.trim(), name == 'on');
            return;
        case 'js':
            doJs(flow, args);
            return;
        default:
            __cxx.log('[提示] 命令 @' + name + ' 暂不支持，已跳过');
            return;
    }
}

// ---------- 对外入口 ----------

function __start(name, source) {
    startFlow(String(name), String(source));
}

function __tick() {
    refreshState();
    for (var i = __flows.length - 1; i >= 0; i--) {
        var flow = __flows[i];
        _curFlow = flow;
        if (tickFlow(flow)) { __flows.splice(i, 1); }
    }
    _curFlow = null;
}

function __feed_text(text) {
    if (!text) return;
    for (var k = 0; k < __flows.length; k++) {
        var f = __flows[k];
        if (f.wait && f.wait.t == 'tip') {
            try { if (f.wait.regex.test(text)) f.wait.hit = true; }
            catch (e) { f.wait.hit = true; }
        }
    }
}
