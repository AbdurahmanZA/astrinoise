<?php
// Astri-noise call filter page backend (ASTRINOISE inside Asterisk, func_astrinoise).
// The switches live in Asterisk's own database (AstDB), read by the dialplan hook just before a trunk
// is dialled (extensions_override_freepbx.conf, [rnnoise-decide]):
//   rnnoise/mode = off|all|some   rnnoise/strength = 1-100   rnnoise/ext/<phone> = on|off
//   rnnoise/max = most cleaned calls at once (0 or missing = no limit)
//   GET  -> {mode, strength, max, phones{ext: on|off}, module{loaded}, stats{...}}
//   POST -> {"op":"mode","mode":"all"} | {"op":"strength","value":80} | {"op":"max","value":10} | {"op":"phones","exts":[...],"state":"on|off|clear"}
// Every change is written to /var/log/asterisk/rnnoise-dash.log with the browser's address.
header('Content-Type: application/json');
header('Cache-Control: no-store');

function ast($cmd) {
    return (string) shell_exec('/usr/sbin/asterisk -rx ' . escapeshellarg($cmd) . ' 2>/dev/null');
}
function fail($code, $msg) {
    http_response_code($code);
    echo json_encode(array('ok' => false, 'error' => $msg));
    exit;
}
function audit($what) {
    @file_put_contents('/var/log/asterisk/rnnoise-dash.log',
        date('Y-m-d H:i:s') . ' ' . ($_SERVER['REMOTE_ADDR'] ?? '?') . ' ' . $what . "\n", FILE_APPEND);
}
function state() {
    $mode = 'off'; $strength = 100; $max = 0; $phones = array();
    foreach (explode("\n", ast('database show rnnoise')) as $line) {
        if (preg_match('#^/rnnoise/mode\s*:\s*(\w+)#', $line, $m)) $mode = $m[1];
        elseif (preg_match('#^/rnnoise/strength\s*:\s*(\d+)#', $line, $m)) $strength = (int) $m[1];
        elseif (preg_match('#^/rnnoise/max\s*:\s*(\d+)#', $line, $m)) $max = (int) $m[1];
        elseif (preg_match('#^/rnnoise/ext/(\d{2,8})\s*:\s*(on|off)#', $line, $m)) $phones[$m[1]] = $m[2];
    }
    $stats = array();
    if (preg_match_all('/(\w+)=([\d.]+)/', ast('astrinoise show stats'), $mm, PREG_SET_ORDER)) {
        foreach ($mm as $m) $stats[$m[1]] = $m[2] + 0;
    }
    return array('ok' => true, 'mode' => in_array($mode, array('off', 'all', 'some'), true) ? $mode : 'off',
        'strength' => $strength, 'max' => $max, 'phones' => $phones,
        'module' => array('loaded' => strpos(ast('module show like func_astrinoise'), 'Running') !== false),
        'stats' => $stats, 'at' => date('Y-m-d H:i:s'));
}

if ($_SERVER['REQUEST_METHOD'] === 'GET') {
    echo json_encode(state());
    exit;
}
if ($_SERVER['REQUEST_METHOD'] !== 'POST') fail(405, 'GET or POST');
// Only the page's own fetch() sends this header; a plain cross-site form post can't.
if (($_SERVER['HTTP_X_REQUESTED_WITH'] ?? '') !== 'callfilter') fail(403, 'missing header');
$op = json_decode(file_get_contents('php://input', false, null, 0, 32000), true);
if (!is_array($op)) fail(400, 'bad request');

switch ($op['op'] ?? '') {
case 'mode':
    $mode = $op['mode'] ?? '';
    if (!in_array($mode, array('off', 'all', 'some'), true)) fail(400, 'mode must be off, all or some');
    ast("database put rnnoise mode $mode");
    audit("mode $mode");
    break;
case 'strength':
    $v = $op['value'] ?? null;
    if (!is_int($v) || $v < 1 || $v > 100) fail(400, 'strength must be 1-100');
    ast("database put rnnoise strength $v");
    audit("strength $v");
    break;
case 'max':
    $v = $op['value'] ?? null;
    if (!is_int($v) || $v < 0 || $v > 500) fail(400, 'max must be 0-500 (0 = no limit)');
    ast("database put rnnoise max $v");
    audit("max $v");
    break;
case 'phones':
    $exts = $op['exts'] ?? null;
    $st = $op['state'] ?? '';
    if (!is_array($exts) || count($exts) < 1 || count($exts) > 1000) fail(400, 'exts must be a list of 1-1000 phones');
    if (!in_array($st, array('on', 'off', 'clear'), true)) fail(400, 'state must be on, off or clear');
    foreach ($exts as $e) {
        if (!is_string($e) || !preg_match('/^\d{2,8}$/', $e)) fail(400, 'a phone is 2-8 digits');
    }
    foreach ($exts as $e) {
        ast($st === 'clear' ? "database del rnnoise ext/$e" : "database put rnnoise ext/$e $st");
    }
    audit(count($exts) . " phone(s) $st: " . implode(',', array_slice($exts, 0, 20)) . (count($exts) > 20 ? ',...' : ''));
    break;
default:
    fail(400, 'unknown change');
}
echo json_encode(state());
