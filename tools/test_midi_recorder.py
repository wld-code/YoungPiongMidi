#!/usr/bin/env python3
"""
test_midi_recorder.py - headless tests for midi_recorder.py. Run with:
python3 tools/test_midi_recorder.py
"""
import os
import sys
import tempfile
import time

from midi_recorder import MidiRecorder, MidiTake, MidiTakePlayer, LiveRecordSession, save_midi_file, _vlq

failures = []


def check(name, condition, detail=""):
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}" + (f" - {detail}" if detail and not condition else ""))
    if not condition:
        failures.append(name)


# --- MidiRecorder --------------------------------------------------------

def test_recorder_ignores_events_while_inactive():
    r = MidiRecorder()
    r.ingest([{"t": time.time(), "kind": "note_on", "note": 60, "velocity": 100}])
    take = r.stop()
    check("ingest() before start() is ignored", take.is_empty())


def test_recorder_captures_relative_timing():
    r = MidiRecorder()
    r.start()
    t0 = r._armed_at
    r.ingest([
        {"t": t0 + 0.10, "kind": "note_on", "note": 60, "velocity": 100},
        {"t": t0 + 0.30, "kind": "note_off", "note": 60, "velocity": 0},
        {"t": t0 + 0.35, "kind": "note_on", "note": 64, "velocity": 90},
    ])
    take = r.stop()
    # rel_t=0 for the *first captured event*, not "0.10s after arm()" -
    # _start_time is lazy (set to that first event's own timestamp, not
    # to time.time() at start()) specifically so a note that already
    # happened microseconds before start() was even called (a real,
    # observed bug with the sequencer's own pattern notes right at a
    # bank transition) isn't mistaken for "before this segment began"
    # and dropped - see MidiRecorder.start()'s docstring. Tolerance is
    # 0.1ms, not the ideal-arithmetic 0, because these are real
    # time.time() wall-clock magnitudes (~1.7e9s since epoch) - float64
    # loses precision below the microsecond level at that magnitude
    # before ingest() even runs its own subtraction; confirmed directly,
    # not a recorder bug. Still far tighter than anything that matters
    # here (this app's own redraw tick runs at ~33ms).
    check("captured 3 events", len(take.events) == 3, f"got {take.events}")
    check("first event's relative time is 0 (it defines this segment's own t=0)",
          abs(take.events[0][0] - 0.0) < 1e-4, f"got {take.events[0]}")
    check("events preserved in order", [e[1] for e in take.events] == ["note_on", "note_off", "note_on"])
    check("note/velocity preserved", take.events[0][2] == 60 and take.events[0][3] == 100)
    check("duration is relative to the first event, not to armed_at",
          abs(take.duration - 0.25) < 1e-4, f"got {take.duration}")
    check("note_on_count counts only note_on", take.note_on_count() == 2)


def test_recorder_ignores_non_note_events():
    r = MidiRecorder()
    r.start()
    t0 = r._armed_at
    r.ingest([
        {"t": t0 + 0.1, "kind": "cc", "controller": 11, "value": 90},
        {"t": t0 + 0.2, "kind": "note_on", "note": 60, "velocity": 100},
    ])
    take = r.stop()
    check("CC events are not captured (this records notes, not full MIDI)", len(take.events) == 1)
    check("the one captured event is the note_on", take.events[0][1] == "note_on")


def test_recorder_never_double_ingests_the_same_event():
    """Callers re-pass an overlapping/growing event log every tick (the
    same pattern NoteHistory uses) - ingest() must dedupe by timestamp,
    not just append everything it's handed."""
    r = MidiRecorder()
    r.start()
    t0 = r._armed_at
    batch1 = [{"t": t0 + 0.1, "kind": "note_on", "note": 60, "velocity": 100}]
    r.ingest(batch1)
    batch2 = batch1 + [{"t": t0 + 0.2, "kind": "note_off", "note": 60, "velocity": 0}]
    r.ingest(batch2)  # batch1's event reappears here - must not be captured twice
    take = r.stop()
    check("overlapping ingest() calls do not duplicate events", len(take.events) == 2,
          f"got {take.events}")


def test_recorder_stop_resets_for_a_fresh_recording():
    r = MidiRecorder()
    r.start()
    t0 = r._armed_at
    r.ingest([{"t": t0 + 0.1, "kind": "note_on", "note": 60, "velocity": 100}])
    first = r.stop()
    check("first take captured", len(first.events) == 1)
    check("recorder inactive after stop()", not r.active)

    r.start()
    t1 = r._armed_at
    r.ingest([{"t": t1 + 0.1, "kind": "note_on", "note": 72, "velocity": 80}])
    second = r.stop()
    check("second recording starts clean, not appended to the first",
          len(second.events) == 1 and second.events[0][2] == 72, f"got {second.events}")


def test_elapsed_reports_zero_when_not_recording():
    r = MidiRecorder()
    check("elapsed() is 0 before any start()", r.elapsed() == 0.0)
    r.start()
    check("elapsed() is >= 0 while active", r.elapsed() >= 0.0)
    r.stop()
    check("elapsed() is 0 again after stop()", r.elapsed() == 0.0)


def test_recorder_captures_an_event_logged_before_start_was_called():
    """The exact regression this fix targets: a background thread (the
    sequencer's own pattern, in production) can log an event via
    engine.note_on() microseconds before start() actually gets called
    and runs - that event's timestamp is then technically *earlier*
    than time.time() read inside start(). The old, eager
    _start_time=time.time() made that event look like it happened
    "before this segment began" (a negative relative time) and get
    silently dropped by the rel_t<0 guard. _start_time is now lazy
    (set to the first *captured* event's own timestamp), so this can't
    happen - there's no eager cutoff to be on the wrong side of.

    Isolates the _start_time concern specifically from the *dedup*
    cursor (_last_seen_ts): a plain start() with no override correctly
    still excludes events from further in the past (see
    test_recorder_ignores_events_while_inactive and
    LiveRecordSession's own carry-forward tests for that) - passing an
    explicit last_seen_ts here, earlier than the event, is what
    set_active_bank() does for a real bank switch, and is what actually
    exercises the bug this fixes."""
    r = MidiRecorder()
    event_time = time.time() - 0.005  # already happened, 5ms in the past, before start() below
    r.start(last_seen_ts=event_time - 0.01)  # mimics set_active_bank()'s carried-forward cursor
    r.ingest([{"t": event_time, "kind": "note_on", "note": 66, "velocity": 100}])
    take = r.stop()
    check("an event timestamped before start() was called is still captured, not dropped",
          take.note_on_count() == 1 and not take.is_empty(), f"got {take.events}")
    check("that event correctly becomes this segment's own t=0",
          take.events and take.events[0][0] == 0.0, f"got {take.events}")


# --- MidiTakePlayer --------------------------------------------------------

def test_player_replays_take_with_correct_order_and_pairing():
    take = MidiTake([
        (0.0, "note_on", 60, 100),
        (0.05, "note_off", 60, 0),
        (0.06, "note_on", 64, 90),
        (0.11, "note_off", 64, 0),
    ])
    events = []
    finished = {"called": False}
    player = MidiTakePlayer(lambda n, v: events.append(("on", n, v)),
                             lambda n: events.append(("off", n)),
                             on_finished=lambda: finished.__setitem__("called", True))
    player.play(take)
    check("playing is True right after play()", player.playing)
    player._thread.join(timeout=5.0)

    check("all 4 events fired in the recorded order",
          events == [("on", 60, 100), ("off", 60), ("on", 64, 90), ("off", 64)], f"got {events}")
    check("on_finished was called", finished["called"])
    check("playing is False once playback completes naturally", not player.playing)


def test_player_stop_mid_playback_releases_held_notes():
    take = MidiTake([
        (0.0, "note_on", 55, 100),
        (2.0, "note_off", 55, 0),  # far in the future - we'll stop long before this
    ])
    events = []
    player = MidiTakePlayer(lambda n, v: events.append(("on", n)), lambda n: events.append(("off", n)))
    player.play(take)
    time.sleep(0.05)  # well inside the long gap before the note_off
    player.stop(join=True)
    check("stopping mid-take still releases the held note (no stuck note)",
          events == [("on", 55), ("off", 55)], f"got {events}")


def test_player_empty_take_is_a_no_op():
    player = MidiTakePlayer(lambda n, v: None, lambda n: None)
    player.play(MidiTake([]))
    check("playing an empty take never starts a thread", not player.playing and player._thread is None)


def test_player_restart_while_previous_still_alive_leaves_no_orphan():
    take = MidiTake([(0.0, "note_on", 40, 100), (2.0, "note_off", 40, 0)])
    events = []
    player = MidiTakePlayer(lambda n, v: events.append(("on", n)), lambda n: events.append(("off", n)))
    player.play(take)
    first_thread = player._thread
    check("first thread alive right after play()", first_thread.is_alive())

    player.play(take)  # restart with no intervening stop() - the adversarial case
    check("restart replaced the thread object", player._thread is not first_thread)
    check("old thread actually exited (no orphan)", not first_thread.is_alive())
    player.stop(join=True)


def test_player_loop_true_repeats_until_stopped():
    take = MidiTake([(0.0, "note_on", 60, 100), (0.01, "note_off", 60, 0)])
    events = []
    player = MidiTakePlayer(lambda n, v: events.append(("on", n)), lambda n: events.append(("off", n)))
    player.play(take, loop=True)
    time.sleep(0.06)  # enough real time for several ~0.01s loops to have happened
    still_playing = player.playing
    player.stop(join=True)

    note_on_count = sum(1 for e in events if e[0] == "on")
    check("loop=True keeps the player running past a single pass", still_playing)
    check("loop=True fires the take's note_on more than once", note_on_count > 1,
          f"got {note_on_count} note_on events")
    check("stop() ends the loop (playing False afterward)", not player.playing)


def test_player_loop_false_default_still_plays_once_and_stops():
    take = MidiTake([(0.0, "note_on", 60, 100), (0.01, "note_off", 60, 0)])
    events = []
    finished = {"called": False}
    player = MidiTakePlayer(lambda n, v: events.append(("on", n)), lambda n: events.append(("off", n)),
                             on_finished=lambda: finished.__setitem__("called", True))
    player.play(take)  # loop defaults to False
    player._thread.join(timeout=2.0)
    check("default (loop=False) plays exactly one pass", sum(1 for e in events if e[0] == "on") == 1)
    check("default (loop=False) calls on_finished and stops on its own", finished["called"] and not player.playing)


# --- LiveRecordSession -----------------------------------------------------

def test_live_session_arm_records_into_current_bank():
    session = LiveRecordSession()
    check("starts unarmed on bank 0 (bank 1)", not session.armed and session.current_bank == 0)
    session.arm()
    check("armed after arm()", session.armed)
    t0 = session._recorder._armed_at
    session.ingest([{"t": t0 + 0.05, "kind": "note_on", "note": 60, "velocity": 100}])
    result = session.disarm()
    check("disarm() returns the finalized (bank, take) when something was recorded",
          result is not None and result[0] == 0, f"got {result}")
    check("the returned take actually has the note", result[1].note_on_count() == 1)
    check("disarmed after disarm()", not session.armed)


def test_live_session_arm_with_bank_index_primes_the_target_bank():
    """Regression test: arm() must let the caller override which bank
    the very first captured note is attributed to. Without this, a
    stale current_bank (left over from an earlier set_active_bank()
    call - e.g. a manual bank-button click made minutes before) would
    silently attribute freshly-armed notes to the wrong bank until the
    next set_active_bank() call happened to correct it - exactly what
    happened with chain-mode playback, which always starts at bank 0
    regardless of which bank was last manually selected."""
    session = LiveRecordSession()
    session.set_active_bank(5)  # simulate an earlier manual bank click, unrelated to this arm
    check("current_bank reflects the stale manual selection before arming", session.current_bank == 5)

    session.arm(bank_index=0)  # e.g. chain playback just started fresh at bank 0
    check("arm(bank_index=0) immediately overrides the stale current_bank", session.current_bank == 0)

    t0 = session._recorder._armed_at
    session.ingest([{"t": t0 + 0.02, "kind": "note_on", "note": 61, "velocity": 100}])
    result = session.disarm()
    check("the note captured right after a primed arm() is attributed to the primed bank, not the stale one",
          result is not None and result[0] == 0, f"got {result}")


def test_live_session_arm_without_bank_index_keeps_current_bank_unchanged():
    """arm() with no argument (the plain "just arm on whatever bank is
    currently selected" case) must behave exactly as before this fix -
    current_bank untouched."""
    session = LiveRecordSession()
    session.set_active_bank(3)
    session.arm()
    check("arm() with no bank_index leaves current_bank exactly as it was", session.current_bank == 3)


def test_live_session_bank_switch_while_armed_retargets_and_finalizes():
    session = LiveRecordSession()
    session.arm()
    t0 = session._recorder._armed_at
    session.ingest([{"t": t0 + 0.05, "kind": "note_on", "note": 48, "velocity": 100}])

    result = session.set_active_bank(2)
    check("switching banks while armed finalizes the bank being left (bank 0)",
          result is not None and result[0] == 0 and result[1].note_on_count() == 1, f"got {result}")
    check("current_bank updates to the new bank", session.current_bank == 2)
    check("still armed after a bank switch - recording keeps going, just retargeted", session.armed)

    t1 = session._recorder._armed_at
    session.ingest([{"t": t1 + 0.05, "kind": "note_on", "note": 55, "velocity": 90}])
    result2 = session.disarm()
    check("the segment captured after switching belongs to the new bank",
          result2 is not None and result2[0] == 2 and result2[1].note_on_count() == 1, f"got {result2}")


def test_live_session_bank_switch_captures_a_note_logged_right_at_the_transition():
    """The chain-mode regression this fix targets end to end: a note
    logged essentially *at* the moment of a bank switch (e.g. the new
    bank's own first pattern step, fired by the player thread right as
    the transition happens) must land in the NEW bank's segment, not
    get silently dropped because set_active_bank()'s start() picked
    "now" as this segment's start and that note's timestamp was a
    hair earlier."""
    session = LiveRecordSession()
    session.arm()
    result = session.set_active_bank(1)
    check("test setup: switching from bank 0 with nothing captured returns None", result is None)
    # The carried-forward dedup cursor after the switch - in production
    # this is where set_active_bank() left it (unchanged, since nothing
    # was captured on bank 0). Constructing the event's timestamp
    # relative to *this*, not to wall-clock "now", is what deterministically
    # exercises the bug regardless of how much real time this test itself
    # takes to run between arm() and here: a note whose timestamp only
    # just clears the carried cursor - the same situation as the new
    # bank's own first pattern step firing right at the transition -
    # must still be captured, not dropped because start() would (before
    # this fix) have picked a *later* time.time() as this segment's t=0.
    carried_cursor = session._recorder._last_seen_ts
    session.ingest([{"t": carried_cursor + 0.001, "kind": "note_on", "note": 60, "velocity": 100}])
    result2 = session.disarm()
    check("a note timestamped right at (just after) the switch is still captured for the new bank",
          result2 is not None and result2[0] == 1 and result2[1].note_on_count() == 1, f"got {result2}")


def test_live_session_switching_to_a_bank_with_no_new_notes_reports_nothing():
    """The core safety property: switching banks (or arming) without
    actually playing anything must never look like "a take was
    recorded" to the caller - the caller only overwrites
    self.takes[bank] when it gets a non-None result back."""
    session = LiveRecordSession()
    session.arm()
    # No ingest() calls at all - nothing was ever played on bank 0.
    result = session.set_active_bank(3)
    check("switching banks having recorded nothing returns None, not an empty take",
          result is None)
    check("still armed, now targeting the new bank", session.armed and session.current_bank == 3)

    result2 = session.disarm()
    check("disarming having (still) recorded nothing also returns None", result2 is None)


def test_live_session_set_active_bank_to_same_bank_is_a_no_op():
    session = LiveRecordSession()
    session.arm()
    t0 = session._recorder._armed_at
    session.ingest([{"t": t0 + 0.02, "kind": "note_on", "note": 60, "velocity": 100}])
    result = session.set_active_bank(0)  # already on bank 0
    check("selecting the already-active bank does not finalize/reset anything",
          result is None)
    check("the in-progress capture is untouched (still has its event)",
          len(session._recorder._events) == 1)


def test_live_session_default_bank_is_zero():
    session = LiveRecordSession()
    check("a fresh session defaults to bank 0 (bank 1), matching the app's default selection",
          session.current_bank == 0)


# --- Standard MIDI File writer --------------------------------------------

def _decode_vlq(data: bytes, pos: int):
    value = 0
    while True:
        b = data[pos]
        pos += 1
        value = (value << 7) | (b & 0x7F)
        if not (b & 0x80):
            break
    return value, pos


def _parse_smf(path):
    """A minimal, test-only Standard MIDI File reader - just enough to
    round-trip what save_midi_file() writes, so the test checks actual
    on-disk bytes rather than only midi_recorder.py's own internal
    consistency."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[0:4] == b"MThd"
    header_len = int.from_bytes(data[4:8], "big")
    assert header_len == 6
    fmt = int.from_bytes(data[8:10], "big")
    ntrks = int.from_bytes(data[10:12], "big")
    ppqn = int.from_bytes(data[12:14], "big")
    pos = 14
    assert data[pos:pos + 4] == b"MTrk"
    track_len = int.from_bytes(data[pos + 4:pos + 8], "big")
    pos += 8
    track_end = pos + track_len
    events = []
    tempo_us = None
    tick = 0
    while pos < track_end:
        delta, pos = _decode_vlq(data, pos)
        tick += delta
        status = data[pos]
        if status == 0xFF:  # meta event
            meta_type = data[pos + 1]
            length, pos2 = _decode_vlq(data, pos + 2)
            payload = data[pos2:pos2 + length]
            pos = pos2 + length
            if meta_type == 0x51:
                tempo_us = int.from_bytes(payload, "big")
            elif meta_type == 0x2F:
                break  # end of track
        else:
            note = data[pos + 1]
            vel = data[pos + 2]
            kind = "note_on" if (status & 0xF0) == 0x90 else "note_off"
            events.append((tick, kind, note, vel))
            pos += 3
    return {"format": fmt, "ntrks": ntrks, "ppqn": ppqn, "tempo_us": tempo_us, "events": events}


def test_vlq_round_trips():
    for value in (0, 1, 127, 128, 200, 16383, 16384, 2097151, 2097152, 999999):
        encoded = _vlq(value)
        decoded, pos = _decode_vlq(encoded, 0)
        check(f"VLQ round-trip for {value}", decoded == value and pos == len(encoded),
              f"encoded={encoded!r} decoded={decoded}")


def test_save_midi_file_header_and_tempo():
    take = MidiTake([(0.0, "note_on", 60, 100), (0.5, "note_off", 60, 0)])
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "t.mid")
        save_midi_file(take, path, ppqn=480, bpm=120.0)
        parsed = _parse_smf(path)
        check("format is 0 (single track)", parsed["format"] == 0)
        check("ntrks is 1", parsed["ntrks"] == 1)
        check("PPQN matches what was passed", parsed["ppqn"] == 480)
        check("tempo meta event present and correct for 120 BPM (500000 us/quarter)",
              parsed["tempo_us"] == 500000, f"got {parsed['tempo_us']}")


def test_save_midi_file_preserves_timing_and_notes():
    take = MidiTake([
        (0.0, "note_on", 60, 100),
        (0.5, "note_off", 60, 0),
        (0.75, "note_on", 67, 110),
        (1.25, "note_off", 67, 0),
    ])
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "t.mid")
        save_midi_file(take, path, ppqn=480, bpm=120.0)
        parsed = _parse_smf(path)
        # 480 ppqn @ 120 BPM = 960 ticks/sec.
        expected_ticks = [round(t * 960) for (t, *_rest) in take.events]
        actual_ticks = [e[0] for e in parsed["events"]]
        check("event tick positions match the recorded timing at 960 ticks/sec",
              actual_ticks == expected_ticks, f"expected={expected_ticks} got={actual_ticks}")
        check("note numbers preserved", [e[2] for e in parsed["events"]] == [60, 60, 67, 67])
        check("note_on velocities preserved",
              parsed["events"][0][3] == 100 and parsed["events"][2][3] == 110)
        check("note_off velocities are 0 (conventional)",
              parsed["events"][1][3] == 0 and parsed["events"][3][3] == 0)
        check("event kinds preserved",
              [e[1] for e in parsed["events"]] == ["note_on", "note_off", "note_on", "note_off"])


def test_save_midi_file_clamps_out_of_range_note_and_velocity():
    take = MidiTake([(0.0, "note_on", 200, 300), (0.1, "note_off", -5, 0)])
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "t.mid")
        save_midi_file(take, path)
        parsed = _parse_smf(path)
        check("out-of-range note is clamped to 127", parsed["events"][0][2] == 127)
        check("out-of-range velocity is clamped to 127", parsed["events"][0][3] == 127)
        check("negative note is clamped to 0", parsed["events"][1][2] == 0)


def test_save_midi_file_empty_take_is_still_a_valid_file():
    take = MidiTake([])
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "empty.mid")
        save_midi_file(take, path)
        parsed = _parse_smf(path)
        check("an empty take still produces a valid, parseable MIDI file (just the tempo meta)",
              parsed["events"] == [] and parsed["tempo_us"] == 500000)


def main():
    test_recorder_ignores_events_while_inactive()
    test_recorder_captures_relative_timing()
    test_recorder_ignores_non_note_events()
    test_recorder_never_double_ingests_the_same_event()
    test_recorder_stop_resets_for_a_fresh_recording()
    test_elapsed_reports_zero_when_not_recording()
    test_recorder_captures_an_event_logged_before_start_was_called()

    test_player_replays_take_with_correct_order_and_pairing()
    test_player_stop_mid_playback_releases_held_notes()
    test_player_empty_take_is_a_no_op()
    test_player_restart_while_previous_still_alive_leaves_no_orphan()
    test_player_loop_true_repeats_until_stopped()
    test_player_loop_false_default_still_plays_once_and_stops()

    test_live_session_arm_records_into_current_bank()
    test_live_session_arm_with_bank_index_primes_the_target_bank()
    test_live_session_arm_without_bank_index_keeps_current_bank_unchanged()
    test_live_session_bank_switch_while_armed_retargets_and_finalizes()
    test_live_session_bank_switch_captures_a_note_logged_right_at_the_transition()
    test_live_session_switching_to_a_bank_with_no_new_notes_reports_nothing()
    test_live_session_set_active_bank_to_same_bank_is_a_no_op()
    test_live_session_default_bank_is_zero()

    test_vlq_round_trips()
    test_save_midi_file_header_and_tempo()
    test_save_midi_file_preserves_timing_and_notes()
    test_save_midi_file_clamps_out_of_range_note_and_velocity()
    test_save_midi_file_empty_take_is_still_a_valid_file()

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED:")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
