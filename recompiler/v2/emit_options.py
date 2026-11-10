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
