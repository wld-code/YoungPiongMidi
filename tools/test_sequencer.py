#!/usr/bin/env python3
"""
test_sequencer.py - headless tests for sequencer.py (the step
sequencer's data model and playback timing) - no Tk, no audio device,
no board. Run with: python3 tools/test_sequencer.py
"""
import sys
import time

from sequencer import (SequencerModel, SequencerPlayer, StepRecorder, Step, step_duration_seconds,
                        clamp_bpm, NUM_BANKS, NUM_STEPS, MIN_BPM, MAX_BPM,
                        DEFAULT_VELOCITY, ACCENT_VELOCITY, STEP_RECORD_ACCENT_VELOCITY_THRESHOLD)

failures = []


def check(name, condition, detail=""):
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}" + (f" - {detail}" if detail and not condition else ""))
    if not condition:
        failures.append(name)


def test_step_duration_math():
    # 120 BPM: 0.5s/beat, 4 steps/beat -> 0.125s/step.
    check("step_duration_seconds(120) == 0.125", abs(step_duration_seconds(120) - 0.125) < 1e-9,
          f"got {step_duration_seconds(120)}")
    # 60 BPM: 1s/beat -> 0.25s/step.
    check("step_duration_seconds(60) == 0.25", abs(step_duration_seconds(60) - 0.25) < 1e-9)
    # Doubling BPM halves step duration.
    d1 = step_duration_seconds(100)
    d2 = step_duration_seconds(200)
    check("doubling BPM halves step duration", abs(d1 / 2 - d2) < 1e-9, f"{d1} vs {d2}")
    # Clamping.
    check("BPM clamps below MIN_BPM", clamp_bpm(1) == MIN_BPM)
    check("BPM clamps above MAX_BPM", clamp_bpm(9999) == MAX_BPM)
    check("in-range BPM unchanged", clamp_bpm(140) == 140)


def test_model_basics():
    m = SequencerModel()
    check("starts with NUM_BANKS banks", len(m.banks) == NUM_BANKS)
    check("each bank has NUM_STEPS steps", all(len(b) == NUM_STEPS for b in m.banks))
    check("all steps start off", all(not s.on for b in m.banks for s in b))
    check("bank 0 selected initially", m.active_bank == 0)

    m.select_bank(3)
    check("select_bank changes active_bank", m.active_bank == 3)
    m.select_bank(99)
    check("select_bank ignores out-of-range index", m.active_bank == 3)
    m.select_bank(-1)
    check("select_bank ignores negative index", m.active_bank == 3)

    step = m.toggle_step(0, 5, note=64)
    check("toggle_step turns a step on with the given note",
          step.on and step.note == 64 and not step.accent)
    check("toggle_step's returned step is the same object stored in the bank",
          m.banks[0][5] is step)

    step2 = m.toggle_step(0, 5, note=70)
    check("toggling an on step turns it off (note argument ignored when turning off)",
          not step2.on)

    m.toggle_step(0, 5, note=70)
    accented = m.toggle_accent(0, 5)
    check("toggle_accent sets accent on an on step", accented.accent)
    check("velocity() reflects accent", accented.velocity() == ACCENT_VELOCITY)
    m.toggle_accent(0, 5)
    check("toggle_accent toggles back off", not m.banks[0][5].accent)

    off_step = m.banks[1][2]
    check("toggle_accent on an off step is a no-op", not off_step.on)
    m.toggle_accent(1, 2)
    check("toggle_accent never turns an off step's accent on", not m.banks[1][2].accent)
    check("default velocity for a non-accented step", Step(on=True).velocity() == DEFAULT_VELOCITY)

    m.toggle_step(2, 0, note=48)
    m.toggle_step(2, 1, note=50)
    m.clear_bank(2)
    check("clear_bank resets every step in that bank", all(not s.on for s in m.banks[2]))
    check("clear_bank does not touch other banks", m.banks[0][5].on)

    m.set_bpm(500)
    check("set_bpm clamps", m.bpm == MAX_BPM)


def test_set_step_overwrite_semantics():
    m = SequencerModel()
    step = m.set_step(0, 3, note=67)
    check("set_step turns a step on with the given note", step.on and step.note == 67 and not step.accent)
    check("set_step's returned step is the same object stored in the bank", m.banks[0][3] is step)

    step2 = m.set_step(0, 3, note=70)
    check("set_step on an already-on step overwrites (not toggles off)", step2.on and step2.note == 70)

    step3 = m.set_step(0, 3, note=72, accent=True)
    check("set_step can set accent explicitly", step3.accent)
    step4 = m.set_step(0, 3, note=74)
    check("a later set_step without accent=True clears any previous accent", not step4.accent)

    check("set_step never touches any other step", not m.banks[0][2].on and not m.banks[0][4].on)


def test_player_plays_correct_pattern_in_order():
    m = SequencerModel()
    m.set_bpm(MAX_BPM)  # fastest supported tempo, keeps this test's wall-clock time small
    # A simple 4-on-16 pattern with one accented step.
    m.toggle_step(0, 0, note=36)
    m.toggle_step(0, 4, note=40)
    m.toggle_step(0, 8, note=43)
    m.toggle_accent(0, 8)
    m.toggle_step(0, 12, note=48)

    events = []
    step_hits = []

    def note_on(note, vel):
        events.append(("on", note, vel))

    def note_off(note):
        events.append(("off", note))

    def on_step(i):
        # Stop deterministically right after the last step of exactly
        # one full loop fires, rather than racing a sleep duration
        # against real thread-scheduling jitter (flaky: a sleep timed
        # at "one loop's worth" can still let the first note_on of the
        # *next* loop sneak in before the thread actually stops).
        step_hits.append(i)
        if i == NUM_STEPS - 1:
            player.stop()

    player = SequencerPlayer(m, note_on, note_off, on_step=on_step)
    player.start()
    player._thread.join(timeout=5.0)

    note_on_events = [e for e in events if e[0] == "on"]
    check("exactly 4 note_on events fired for the 4 programmed steps",
          len(note_on_events) == 4, f"got {note_on_events}")
    check("notes fired in program order", [e[1] for e in note_on_events] == [36, 40, 43, 48],
          f"got {[e[1] for e in note_on_events]}")
    check("accented step used ACCENT_VELOCITY, others DEFAULT_VELOCITY",
          [e[2] for e in note_on_events] == [DEFAULT_VELOCITY, DEFAULT_VELOCITY, ACCENT_VELOCITY, DEFAULT_VELOCITY],
          f"got {[e[2] for e in note_on_events]}")

    # Every note_on must be followed (eventually, before the next one)
    # by a note_off for that same note - no stuck notes.
    on_notes_seen = []
    ok_pairing = True
    i = 0
    while i < len(events):
        kind = events[i]
        if kind[0] == "on":
            note = kind[1]
            if i + 1 >= len(events) or events[i + 1] != ("off", note):
                ok_pairing = False
                break
        i += 1
    check("every note_on is immediately followed by its own note_off (proper gating)", ok_pairing)

    check("on_step visited all 16 step indices at least once",
          set(range(NUM_STEPS)) <= set(step_hits), f"got {sorted(set(step_hits))}")
    check("player.playing is False after stop()", not player.playing)
    check("player.current_step resets to -1 after stop()", player.current_step == -1)


def test_bank_switch_mid_playback_takes_effect_next_step():
    m = SequencerModel()
    m.set_bpm(MAX_BPM)
    m.toggle_step(0, 0, note=60)  # bank 0: only step 0 on
    m.toggle_step(1, 0, note=72)  # bank 1: only step 0 on, different note

    events = []
    switched = {"done": False}

    def note_on(note, vel):
        del vel
        events.append(note)

    def note_off(note):
        del note

    def on_step(i):
        # Switch banks deterministically right after step 0 of the
        # first loop fires, instead of racing a sleep against playback.
        if i == 0 and not switched["done"]:
            switched["done"] = True
            m.select_bank(1)

    player = SequencerPlayer(m, note_on, note_off, on_step=on_step)
    player.start()
    time.sleep(step_duration_seconds(MAX_BPM) * (NUM_STEPS + NUM_STEPS // 2) * 1.1)
    player.stop(join=True)

    check("first loop played bank 0's note (60)", events[0] == 60 if events else False,
          f"events={events}")
    check("after switching mid-playback, a later loop played bank 1's note (72)",
          72 in events[1:], f"events={events}")


def test_stop_turns_off_a_currently_held_note():
    m = SequencerModel()
    m.set_bpm(MIN_BPM)  # slow, so we can stop reliably mid-gate
    m.toggle_step(0, 0, note=55)

    events = []
    player = SequencerPlayer(m, lambda n, v: events.append(("on", n)),
                              lambda n: events.append(("off", n)))
    player.start()
    time.sleep(step_duration_seconds(MIN_BPM) * 0.2)  # well inside step 0's gate
    player.stop(join=True)

    check("stopping mid-note still sends a matching note_off (no stuck note)",
          events == [("on", 55), ("off", 55)], f"got {events}")


def test_rapid_stop_then_start_does_not_silently_no_op():
    """A GUI Play/Stop button reads player.playing to decide what to do
    next; stop() must make that flip visible synchronously so an
    immediate re-click of Play actually starts a new run rather than
    silently doing nothing because the old thread hadn't finished
    unwinding yet."""
    m = SequencerModel()
    m.set_bpm(MIN_BPM)  # slow, so the first run is still very much in progress when we stop it
    m.toggle_step(0, 0, note=61)

    events = []
    player = SequencerPlayer(m, lambda n, v: events.append(n), lambda n: None)
    player.start()
    check("playing is True right after start()", player.playing)
    player.stop()
    check("playing is False immediately after stop(), before the thread has unwound", not player.playing)

    player.start()
    check("start() right after stop() actually starts a new run (playing True again)", player.playing)
    player.stop(join=True)


def test_restart_while_previous_thread_still_alive_leaves_no_orphan():
    """Calling start() again while the previous run's thread is still
    genuinely alive (not just "playing flag not yet flipped" - the
    actual OS thread mid-wait()) must cleanly replace it, not leave an
    orphaned thread that missed the stop signal and keeps firing
    concurrently with the new one."""
    m = SequencerModel()
    m.set_bpm(MIN_BPM)  # slow steps, guarantees the old thread is still mid-wait() below
    m.toggle_step(0, 0, note=44)

    events = []
    player = SequencerPlayer(m, lambda n, v: events.append(("on", n)),
                              lambda n: events.append(("off", n)))
    player.start()
    first_thread = player._thread
    check("first thread is alive right after start()", first_thread.is_alive())

    player.start()  # restart with no intervening stop() - the adversarial case
    check("restarting replaced the thread object", player._thread is not first_thread)
    # Give the old thread's join() (inside the second start()) time to
    # have actually completed - start() itself blocks on that, so by
    # the time start() returns this should already be guaranteed, but
    # double-check explicitly since that guarantee is the entire point
    # of this test.
    check("the old thread actually exited (no orphan left running)", not first_thread.is_alive())

    player.stop(join=True)
    # Same pairing invariant as the main playback test: no unpaired/
    # interleaved note_on from two generations running concurrently.
    ok_pairing = True
    i = 0
    while i < len(events):
        if events[i][0] == "on":
            note = events[i][1]
            if i + 1 >= len(events) or events[i + 1] != ("off", note):
                ok_pairing = False
                break
        i += 1
    check("no interleaved/unpaired events from an orphaned old-generation thread", ok_pairing,
          f"events={events}")


# --- StepRecorder ("record my voice/playing into the pattern") ------------

def _evt(rec, delta, note, velocity=100, source=None):
    """Builds a synthetic event timestamped relative to `rec`'s own
    ingestion cursor (which arm() sets to a real time.time(), same as
    production) rather than an arbitrary small literal - StepRecorder's
    dedup compares against real wall-clock magnitudes, so a literal
    like t=1.0 would already be "in the past" the instant arm() runs
    and get silently skipped, same reasoning as the wall-clock
    precision note on MidiRecorder's own tests in test_midi_recorder.py."""
    e = {"t": rec._last_seen_ts + delta, "kind": "note_on", "note": note, "velocity": velocity}
    if source is not None:
        e["source"] = source
    return e


def test_step_recorder_writes_external_note_into_current_step():
    m = SequencerModel()
    rec = StepRecorder(m)
    check("starts unarmed", not rec.armed)
    rec.arm()
    check("armed after arm()", rec.armed)

    rec.ingest([_evt(rec, 0.01, note=64)], current_step=5)
    step = m.banks[0][5]
    check("an external note_on lands on the given current_step", step.on and step.note == 64,
          f"got on={step.on} note={step.note}")
    check("no other step was touched", not any(m.banks[0][i].on for i in range(NUM_STEPS) if i != 5))


def test_step_recorder_ignores_pattern_and_take_playback_sourced_notes():
    m = SequencerModel()
    rec = StepRecorder(m)
    rec.arm()
    rec.ingest([_evt(rec, 0.01, note=60, source="pattern")], current_step=3)
    check("a source='pattern' note_on is never written into the grid (no self-write-back)",
          not m.banks[0][3].on)

    rec.ingest([_evt(rec, 0.02, note=61, source="take_playback")], current_step=3)
    check("a source='take_playback' note_on is also ignored (no take-of-a-take loop)",
          not m.banks[0][3].on)

    rec.ingest([_evt(rec, 0.03, note=62)], current_step=3)  # no source key at all -> defaults to external
    check("a note_on with no source key at all defaults to external and IS recorded",
          m.banks[0][3].on and m.banks[0][3].note == 62)


def test_step_recorder_ignores_note_off_and_non_note_events():
    m = SequencerModel()
    rec = StepRecorder(m)
    rec.arm()
    rec.ingest([{"t": rec._last_seen_ts + 0.01, "kind": "note_off", "note": 60}], current_step=2)
    rec.ingest([{"t": rec._last_seen_ts + 0.02, "kind": "cc", "controller": 11, "value": 90}],
               current_step=2)
    check("note_off and cc events never write into the grid", not m.banks[0][2].on)


def test_step_recorder_ignored_while_disarmed():
    m = SequencerModel()
    rec = StepRecorder(m)
    rec.ingest([_evt(rec, 0.01, note=64)], current_step=5)  # never armed (_last_seen_ts still 0.0)
    check("ingest() before arm() is a no-op", not m.banks[0][5].on)

    rec.arm()
    rec.disarm()
    rec.ingest([_evt(rec, 0.02, note=64)], current_step=5)
    check("ingest() after disarm() is also a no-op", not m.banks[0][5].on)


def test_step_recorder_ignores_notes_when_pattern_not_running():
    m = SequencerModel()
    rec = StepRecorder(m)
    rec.arm()
    rec.ingest([_evt(rec, 0.01, note=64)], current_step=-1)  # SequencerPlayer.current_step when stopped
    check("current_step=-1 (pattern not running) means nothing gets written anywhere",
          not any(s.on for s in m.banks[0]))


def test_step_recorder_high_velocity_sets_accent():
    m = SequencerModel()
    rec = StepRecorder(m)
    rec.arm()
    rec.ingest([_evt(rec, 0.01, note=60, velocity=STEP_RECORD_ACCENT_VELOCITY_THRESHOLD)], current_step=0)
    check("velocity at the accent threshold sets accent", m.banks[0][0].accent)

    rec.ingest([_evt(rec, 0.02, note=61, velocity=STEP_RECORD_ACCENT_VELOCITY_THRESHOLD - 1)], current_step=1)
    check("velocity just below the threshold does not set accent", not m.banks[0][1].accent)


def test_step_recorder_overwrites_only_when_a_new_note_actually_lands():
    """The literal requirement: switching to (or arming over) a bank/
    step with existing content must never erase it - only an actual new
    note landing on that exact step does."""
    m = SequencerModel()
    m.set_step(0, 4, note=48)  # pre-existing content, as if hand-programmed
    rec = StepRecorder(m)
    rec.arm()

    rec.ingest([_evt(rec, 0.01, note=99)], current_step=7)  # a note lands elsewhere
    check("an existing step is untouched when a note lands on a different step",
          m.banks[0][4].on and m.banks[0][4].note == 48)

    rec.ingest([_evt(rec, 0.02, note=55)], current_step=4)  # a note lands ON the pre-existing step
    check("a new note landing exactly on an existing step legitimately overwrites it",
          m.banks[0][4].on and m.banks[0][4].note == 55)


def test_step_recorder_follows_bank_switches_with_zero_extra_wiring():
    """Unlike LiveRecordSession, StepRecorder never has to be told about
    a bank switch - it always writes into model.active_bank, so
    changing that (as the GUI's bank selector does) is immediately
    reflected on the very next ingest(), with no set_active_bank() call
    needed at all."""
    m = SequencerModel()
    rec = StepRecorder(m)
    rec.arm()
    rec.ingest([_evt(rec, 0.01, note=60)], current_step=0)
    check("recorded into bank 0 (the model's default active bank)", m.banks[0][0].on)

    m.select_bank(3)
    rec.ingest([_evt(rec, 0.02, note=67)], current_step=0)
    check("after the model's active bank changes, the very next note goes to the new bank",
          m.banks[3][0].on and m.banks[3][0].note == 67)
    check("bank 0's earlier content is untouched by the switch", m.banks[0][0].note == 60)


def main():
    test_step_duration_math()
    test_model_basics()
    test_set_step_overwrite_semantics()
    test_player_plays_correct_pattern_in_order()
    test_bank_switch_mid_playback_takes_effect_next_step()
    test_stop_turns_off_a_currently_held_note()
    test_rapid_stop_then_start_does_not_silently_no_op()
    test_restart_while_previous_thread_still_alive_leaves_no_orphan()

    test_step_recorder_writes_external_note_into_current_step()
    test_step_recorder_ignores_pattern_and_take_playback_sourced_notes()
    test_step_recorder_ignores_note_off_and_non_note_events()
    test_step_recorder_ignored_while_disarmed()
    test_step_recorder_ignores_notes_when_pattern_not_running()
    test_step_recorder_high_velocity_sets_accent()
    test_step_recorder_overwrites_only_when_a_new_note_actually_lands()
    test_step_recorder_follows_bank_switches_with_zero_extra_wiring()

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED:")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
