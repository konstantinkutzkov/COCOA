"""Synthetic solver for dry-running the race scheduler — NO real solvers.

Progress advances with CPU WORK (a busy loop), not wall-clock, so SIGSTOP
genuinely freezes progress (a stopped process does no work) and SIGCONT resumes
it — exactly the property the scheduler relies on. After `finish_work` units of
work it prints an engine-appropriate count and exits; pass `none` to never finish.

Usage: python _mock.py <engine> <rate> <finish_work|none> <count>
"""
import sys
import time


def main():
    engine = sys.argv[1]
    rate = float(sys.argv[2])
    finish = sys.argv[3]
    count = sys.argv[4]
    finish_w = None if finish == "none" else float(finish)

    work = 0.0
    t0 = time.monotonic()
    last_emit = -1.0
    while True:
        # a little CPU work; progress is a function of work done, not wall time
        s = 0.0
        for _ in range(50000):
            s += 1e-9
        work += 1.0 + s
        pct = work * rate
        now = time.monotonic() - t0
        if now - last_emit >= 0.25:
            if engine == "cocoa":
                sys.stderr.write(
                    f"PROGRESS t={now:.3f} pct_log=0 pct_lin={pct:.6g} "
                    f"closed_bits={pct * 90.0:.4f} open=1 "
                    f"decisions={int(work * 1000)} l2_hits={int(work * 500)}\n")
                sys.stderr.flush()
            else:
                sys.stdout.write(
                    f"c o cache entries K                {int(work * 10)}\n"
                    f"c o conflicts                      {int(work)}"
                    f"            -- confl/s:    {rate * 100:.2f}\n"
                    f"c o cubes resolved/flp-removed     {int(work * 2)}     / 0\n")
                sys.stdout.flush()
            last_emit = now
        if finish_w is not None and work >= finish_w:
            if engine == "cocoa":
                sys.stdout.write(f"\n# solutions \n{count}\n# END\n")
            else:
                sys.stdout.write(f"s SATISFIABLE\nc s exact arb int {count}\n")
            sys.stdout.flush()
            return
        time.sleep(0.01)


if __name__ == "__main__":
    main()
