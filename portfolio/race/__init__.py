"""Step-2 race: the progress-raced portfolio for UNDECIDED instances.

A sequential, resume-based scheduler that races curated solver archetypes under a
total wall budget, freezing losers with SIGSTOP and resuming the frontrunners with
SIGCONT so their work is never thrown away. See `scheduler.run_race`.
"""
from .scheduler import run_race          # noqa: F401
from .archetypes import STRONG, FILLERS, default_set, Archetype  # noqa: F401
