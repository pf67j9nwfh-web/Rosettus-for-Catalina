#!/usr/bin/python
"""saverhelper.py - the Rosettus screen saver agent (Rosettus for Catalina).

Catalina runs third-party screen savers in legacyScreenSaver, a sandboxed process: a PowerPC
saver's host started from there would be sandboxed as well and could not run the translator. The
native shim saver (src/savers/shim) posts a distributed notification instead, and this agent (a
launch agent, com.rosettus.ppc.saverhelper) starts the host outside the sandbox:

  com.rosettus.saver.start  object: token, runner, host app, Original.saver, x, y, w, h, level
                            (separated by U+001F)
  com.rosettus.saver.stop   object: token

Only Rosettus's own runner and saver host are started, and only for an Original.saver inside an
installed "(PowerPC).saver" shim. A host is also stopped when no screen saver engine runs any more.
Log: ~/Library/Logs/Rosettus/ScreenSaver.log
"""
import os, signal, subprocess, sys, time
from Foundation import NSObject, NSDistributedNotificationCenter, NSRunLoop, NSDate, NSTimer
import objc

ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
RUNNER = os.path.join(ROOT, 'rosettus')
HOSTAPP = os.path.join(ROOT, 'savers', 'Rosettus Saver Host.app')
LOG = os.path.expanduser('~/Library/Logs/Rosettus/ScreenSaver.log')
hosts = {}          # token -> Popen


def log(msg):
    try:
        if not os.path.isdir(os.path.dirname(LOG)):
            os.makedirs(os.path.dirname(LOG))
        with open(LOG, 'a') as f:
            f.write('%s [agent] %s\n' % (time.strftime('%H:%M:%S'), msg))
    except Exception:
        pass


def stop(token):
    p = hosts.pop(token, None)
    if p is None:
        return
    try:
        os.killpg(p.pid, signal.SIGTERM)
    except OSError:
        pass
    for i in range(20):
        if p.poll() is not None:
            break
        time.sleep(0.05)
    if p.poll() is None:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:
            pass
    log('stopped host %d' % p.pid)


def valid_saver(path):
    real = os.path.realpath(path)
    parts = real.split(os.sep)
    return (real.endswith('/Contents/Resources/Original.saver') and os.path.isdir(real)
            and any(p.endswith(' (PowerPC).saver') for p in parts))


def engine_running():
    out = subprocess.Popen(['/usr/bin/pgrep', '-x', 'ScreenSaverEngine'], stdout=subprocess.PIPE).communicate()[0]
    return bool(out.strip())


class Agent(NSObject):
    def start_(self, note):
        parts = (note.object() or u'').split(u'\x1f')
        if len(parts) != 9:
            log('ignored a request with %d fields' % len(parts))
            return
        token, runner, hostapp, orig = parts[:4]
        geom = parts[4:]
        try:
            [float(g) for g in geom]
        except ValueError:
            log('ignored a request with a bad geometry')
            return
        # always our own runner and host, whatever the request named
        if os.path.realpath(hostapp) != os.path.realpath(HOSTAPP):
            log('ignored a request for %s' % hostapp)
            return
        if not valid_saver(orig):
            log('ignored a request for %s' % orig)
            return
        stop(token)
        out = open(LOG, 'a')
        # Catalina shows the screen saver in a space above every app's windows, whatever their
        # level: liboah puts the host's window into a space of its own at absolute level 300 (the
        # screen saver's), created after it and so in front of it
        # OAH_NO_SETFRONT: the engine keeps the foreground, so that a preview started from System
        # Preferences still ends on a mouse move or key press
        env = dict(os.environ, OAH_WINDOW_SPACE_LEVEL='300', OAH_NO_SETFRONT='1')
        p = subprocess.Popen([RUNNER, HOSTAPP, orig] + [str(g) for g in geom], env=env,
                             stdin=open(os.devnull), stdout=out, stderr=out, preexec_fn=os.setpgrp)
        hosts[token] = p
        log('started host %d for %s at %s' % (p.pid, orig, ' '.join(geom)))

    def stop_(self, note):
        stop(note.object() or u'')

    def tick_(self, timer):
        for t, p in list(hosts.items()):
            if p.poll() is not None:
                hosts.pop(t, None)
                log('host %d ended (%s)' % (p.pid, p.returncode))
        if hosts and not engine_running():
            log('no screen saver engine: stopping %d host(s)' % len(hosts))
            for t in list(hosts):
                stop(t)


def main():
    agent = Agent.alloc().init()
    c = NSDistributedNotificationCenter.defaultCenter()
    # NSNotificationSuspensionBehaviorDeliverImmediately = 4
    c.addObserver_selector_name_object_suspensionBehavior_(agent, 'start:', u'com.rosettus.saver.start', None, 4)
    c.addObserver_selector_name_object_suspensionBehavior_(agent, 'stop:', u'com.rosettus.saver.stop', None, 4)
    NSTimer.scheduledTimerWithTimeInterval_target_selector_userInfo_repeats_(2.0, agent, 'tick:', None, True)
    signal.signal(signal.SIGTERM, lambda *a: ([stop(t) for t in list(hosts)], sys.exit(0)))
    log('agent running (pid %d)' % os.getpid())
    while True:
        NSRunLoop.currentRunLoop().runUntilDate_(NSDate.dateWithTimeIntervalSinceNow_(5.0))


if __name__ == '__main__':
    main()
