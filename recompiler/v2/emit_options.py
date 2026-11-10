"""Emitter options read from the environment.

SNESRECOMP_EMIT_LEAN=1 (off by default): smaller generated code with the same
behaviour. Each block's watchdog and deadline checks become one call
(recomp_block_enter), every yield in a function shares one exit, the
code-region speed for bank-mirrored bodies is read from g_code_speed[], and
the void-alias leak check calls one shared fatal routine. Listed in
tools/v2_emit.py EMIT_SWITCHES (the generator digest), so a cached bank is
never reused across modes. Any non-empty value turns it on, as the digest
reads it.
"""
import os


def lean_emit() -> bool:
    return bool(os.environ.get("SNESRECOMP_EMIT_LEAN"))


def code_cache_emit() -> bool:
    """SNESRECOMP_EMIT_CODE_CACHE=1 (with SNESRECOMP_EMIT_LEAN): every
    compiled routine F is emitted as F__body, and dispatch_v2.c defines F as
    a resident trampoline through a RecompRoutine record (runner/src/
    recomp_cache.h), so bodies can be loaded and evicted at run time. Callers,
    the dispatch table and host wrappers keep calling F."""
    return lean_emit() and bool(os.environ.get("SNESRECOMP_EMIT_CODE_CACHE"))

