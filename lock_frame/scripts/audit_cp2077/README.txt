lock_frame consumer self-audit (2026-11-25)
===========================================

What this tests
---------------
lock_frame/docs/SELF_AUDIT_2026-11-25.md, items A/C/D/E/F (consumer only;
producer / fakerFG / protocol unchanged):

  A  queue-side wait on the producer's fwd[slot] before any MV/depth read
  C  upsample pair is keyed by publication, not just by slot number
  D  record_upsample() barrier StateBefore COPY_DEST -> COMMON
  E  prefetch is refused when a codec stream is in use
  F  --lock + --jbuf no longer leaks colour textures

Deployment
----------
The audited consumer is lock_frame.exe in this folder. The producer changed as
well (B/J/R): xess_mfg_producer.dll -> the GAME MUST BE RESTARTED before testing
(the DLL is loaded at process start).
The exact md5 of both files for this round is in audit_logs\audit_deploy_info.txt
(md5 changes on every rebuild because of the PE timestamp; do not compare against
a hardcoded value).

2026-11-25 round 2 (B/J/R):
  B  claim_slot() only reclaims a publication the consumer cannot need
     (frame_id==0, or frame_id+2 <= colour_seq); otherwise it drops the frame.
     Look for "reclaimed SAFE slot" / "no safe slot to reuse".
  J  TransportSlots=16 (fakerFG.ini): only 16 publication slots are placed per
     stream. "shared: TransportSlots=16" and "shared ready ... ring=16 used=..."
     should show roughly half the old mapping size (~890 MB instead of 1774 MB).
  R  the producer stops copying once the consumer process is gone:
     "producer: consumer process is gone - pausing copies after 3 s".
     Leave the overlay (Ctrl+Alt+End) and keep playing: no more codec lines in
     the producer log, and the heartbeat shows publish/skip not advancing.

Order (game already running, in-world, DLSS Frame Generation ON, borderless)
---------------------------------------------------------------------------
  1. audit_0_info.bat          identity + md5 + --version; writes audit_logs\audit_build_info.txt
  2. audit_1_3x_keep2x.bat     baseline A: 3x / keep2x / compat flags; ~60 s, turn the camera
  3. audit_2_3x_keep2x_ui.bat  UI patch path (mode 4, --hudless-stream); ~60 s
  4. audit_3_3x_debug.bat      same as step 1 + --d3d-debug; 30-45 s; validates D
  4b.audit_out_3x_keep2x.bat   same as step 1, but the consumer runs from
                               C:\temp\lock_frame_audit (release layout) so fakerFG is NOT
                               injected into the consumer process. Use this one for a clean
                               A/B; check that no new "==== fakerFG" session appears in
                               lock_frame_fakerFG.log for that start time.
  5. audit_ui_tagdump.bat      UI mode 4 visual dump: 8 consecutive frames of
                               backbuffer / H' / patch / alpha / |P-H'|x8
                               -> audit_logs	agdump
  6. audit_9_report.bat        copies everything to audit_logs\latest + summary.txt

Stop a run with Ctrl+Alt+End (the overlay's own hotkey). Do not use Alt+F4.

What "pass" looks like
----------------------
  pair: ... no_slot=0 ... dup small; exact ~ total
  xefg-in: sizes identical to the 2026-11-25 compat_3x_default run
  breakdown: load / slow / HITCH not worse than the baseline
  audit_3_3x_debug.log: no "statebefore" / "RESOURCE_BARRIER" / "INVALID_CALL"
                        / "DEVICE_REMOVED" lines, and the [d3d12 ...] count is 0
  no "publish: Signal FAILED" in Cyberpunk2077_mfg_producer.log
  no "pf:" lines (prefetch is force-disabled in this build)

If audit_3 shows validation messages
------------------------------------
Send that log. The usual suspects are the two record_upsample() transitions
(now COMMON) or the shared-slot buffer state in the codec decode; the message
text names the resource and the barrier, so it is directly actionable.

Rollback
--------
lock_frame.exe.bak_audit_<timestamp> in this folder is the previously deployed
consumer (md5 299e04f81e5441adde3b2c713306dd3b). Copy it back over
lock_frame.exe to return to the 2026-11-25 compat baseline.
