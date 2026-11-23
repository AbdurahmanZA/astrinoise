<?php
// Phone list for the Astri-noise phones page, read from FreePBX.
// Sources: FreePBX devices + users (MySQL "asterisk"), live PJSIP registrations, logged-on hot-desk
// users (AstDB DEVICE/<id>/user), and CDR for the last call / calls in the last 30 days.
// Read-only. Runs as the FreePBX web user (asterisk), which can read /etc/freepbx.conf and run asterisk -rx.
header('Content-Type: application/json');
header('Cache-Control: no-store');
include('/etc/freepbx.conf');

function ast($cmd) {
    return shell_exec('/usr/sbin/asterisk -rx ' . escapeshellarg($cmd) . ' 2>&1');
}

$host = $amp_conf['AMPDBHOST'] ?: 'localhost';
$db = new mysqli($host, $amp_conf['AMPDBUSER'], $amp_conf['AMPDBPASS'], ($amp_conf['AMPDBNAME'] ?? '') ?: 'asterisk');
if ($db->connect_errno) { http_response_code(500); echo json_encode(array('error' => 'DB connect failed')); exit; }

$rows = array();
$res = $db->query("SELECT id, tech, devicetype, user, description FROM devices");
while ($r = $res->fetch_assoc()) {
    $rows[$r['id']] = array('ext' => $r['id'], 'name' => $r['description'], 'tech' => $r['tech'],
        'owner' => ($r['user'] && $r['user'] !== 'none' && $r['user'] !== $r['id']) ? $r['user'] : '');
}

// Live registrations: "Contact: 1001/sip:1001@203.0.113.5:5060;... <hash> Avail ..."
$reg = array();
foreach (explode("\n", ast('pjsip show contacts')) as $line) {
    if (preg_match('/Contact:\s+([^\/\s]+)\/sip:\S*\s+\S+\s+(\w+)/', $line, $m) && $m[2] !== 'Unavail') $reg[$m[1]] = true;
}

// Hot desk: device -> logged-on user
$loggedOn = array();
foreach (explode("\n", ast('database show DEVICE')) as $line) {
    if (preg_match('#^/DEVICE/([^/]+)/user\s*:\s*(\S+)#', $line, $m) && $m[2] !== 'none' && $m[2] !== $m[1]) $loggedOn[$m[1]] = $m[2];
}

// Last call + calls in the last 30 days per device (by its PJSIP channel name)
$last = array(); $n30 = array();
// CDR is optional: a missing or empty CDR database only leaves the last-call columns blank.
try {
    $cdr = new mysqli($host, $amp_conf['AMPDBUSER'], $amp_conf['AMPDBPASS'], ($amp_conf['CDRDBNAME'] ?? '') ?: 'asteriskcdrdb');
    $q = "SELECT k, MAX(calldate) AS last, SUM(calldate >= NOW() - INTERVAL 30 DAY) AS n30 FROM (
            SELECT SUBSTRING_INDEX(SUBSTRING_INDEX(channel, '-', 1), '/', -1) AS k, calldate FROM cdr
              WHERE channel LIKE 'PJSIP/%' AND calldate >= NOW() - INTERVAL 90 DAY
            UNION ALL
            SELECT SUBSTRING_INDEX(SUBSTRING_INDEX(dstchannel, '-', 1), '/', -1), calldate FROM cdr
              WHERE dstchannel LIKE 'PJSIP/%' AND calldate >= NOW() - INTERVAL 90 DAY
          ) x GROUP BY k";
    if ($res = $cdr->query($q)) {
        while ($r = $res->fetch_assoc()) { $last[$r['k']] = $r['last']; $n30[$r['k']] = (int) $r['n30']; }
    }
} catch (Exception $e) {
    // leave $last / $n30 empty
}

$now = time();
$out = array();
foreach ($rows as $ext => $r) {
    $lc = $last[$ext] ?? null;
    $r['registered'] = isset($reg[$ext]);
    $r['logged_on'] = $loggedOn[$ext] ?? '';
    $r['last_call'] = $lc;
    $r['calls_30d'] = $n30[$ext] ?? 0;
    $r['days_unused'] = $r['registered'] ? 0 : ($lc ? (int) floor(($now - strtotime($lc)) / 86400) : null);
    $out[] = $r;
}
usort($out, function ($a, $b) { return strnatcmp($a['ext'], $b['ext']); });
echo json_encode(array('generated' => date('c'), 'rows' => $out));
