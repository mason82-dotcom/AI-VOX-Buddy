#!/usr/bin/env python3
"""Unit tests for intent_handler.Assistant (no MQTT needed).
Run: python3 rhasspy/test_intent_handler.py"""
import datetime
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import intent_handler as ih  # noqa: E402

NOW = datetime.datetime(2026, 10, 11, 22, 5, 0)


class FakeTimer:
    def __init__(self, seconds, fn, args=()):
        self.seconds, self.fn, self.args = seconds, fn, args
        self.started = self.cancelled = False
        self.daemon = False

    def start(self):
        self.started = True

    def cancel(self):
        self.cancelled = True

    def fire(self):
        self.fn(*self.args)


def event(intent, site="ai-vox3", **slots):
    return {"intent": {"intentName": intent}, "siteId": site, "sessionId": "s1",
            "slots": [{"slotName": k, "value": {"kind": "Unknown", "value": v}} for k, v in slots.items()]}


class AssistantTest(unittest.TestCase):
    def setUp(self):
        self.published = []
        self.timers = []
        self.clock = NOW

        def factory(seconds, fn, args=()):
            timer = FakeTimer(seconds, fn, args)
            self.timers.append(timer)
            return timer

        self.a = ih.Assistant(lambda t, p: self.published.append((t, p)), now=lambda: self.clock,
                              timer_factory=factory)

    def ask(self, intent, **slots):
        return self.a.handle(event(intent, **slots))

    def test_time_and_date(self):
        self.assertEqual(self.ask("GetTime"), "Es ist 22 Uhr 5.")
        self.clock = datetime.datetime(2026, 10, 11, 9, 0)
        self.assertEqual(self.ask("GetTime"), "Es ist 9 Uhr.")
        self.assertEqual(self.ask("GetDate"), "Heute ist Sonntag, der 11. Oktober 2026.")

    def test_days_until(self):
        self.assertEqual(self.ask("DaysUntil", event="weihnachten"), "Bis Weihnachten sind es noch 74 Tage.")
        self.assertEqual(self.ask("DaysUntil", event="neujahr"), "Bis Neujahr sind es noch 82 Tage.")
        self.clock = datetime.datetime(2026, 12, 30, 12, 0)
        self.assertEqual(self.ask("DaysUntil", event="silvester"), "Bis Silvester ist es noch ein Tag.")
        self.clock = datetime.datetime(2026, 12, 24, 12, 0)
        self.assertEqual(self.ask("DaysUntil", event="weihnachten"), "Heute ist Weihnachten!")
        self.clock = datetime.datetime(2026, 12, 25, 12, 0)
        self.assertEqual(self.ask("DaysUntil", event="weihnachten"), "Bis Weihnachten sind es noch 364 Tage.")

    def test_timer(self):
        self.assertEqual(self.ask("TimerStatus"), "Es läuft gerade kein Timer.")
        self.assertEqual(self.ask("SetTimer", amount=5, unit="minuten"), "Timer auf 5 Minuten gestellt.")
        self.assertEqual(self.timers[-1].seconds, 300)
        self.assertTrue(self.timers[-1].started)
        self.clock = NOW + datetime.timedelta(seconds=100)
        self.assertEqual(self.ask("TimerStatus"), "Der Timer läuft noch 3 Minuten und 20 Sekunden.")
        # A new timer replaces the old one.
        self.assertEqual(self.ask("SetTimer", amount="eine", unit="minute"), "Timer auf eine Minute gestellt.")
        self.assertTrue(self.timers[0].cancelled)
        self.timers[-1].fire()
        self.assertEqual(self.published[-1], ("hermes/dialogueManager/startSession", {
            "siteId": "ai-vox3", "init": {"type": "notification", "text": "Der Timer ist abgelaufen."}}))
        self.assertEqual(self.ask("CancelTimer"), "Es läuft gerade kein Timer.")
        self.assertEqual(self.ask("SetTimer", amount=30, unit="sekunden"), "Timer auf 30 Sekunden gestellt.")
        self.assertEqual(self.ask("CancelTimer"), "Timer abgebrochen.")
        self.assertTrue(self.timers[-1].cancelled)

    def test_volume(self):
        self.assertEqual(self.ask("VolumeUp"), "Lautstärke 55 Prozent.")
        self.assertEqual(self.published[-1], ("rhasspy/audioServer/setVolume", {"siteId": "ai-vox3", "volume": 0.55}))
        self.a.on_volume("ai-vox3", 95)
        self.assertEqual(self.ask("VolumeUp"), "Lautstärke 100 Prozent.")
        self.assertEqual(self.ask("SetVolume", volume=20), "Lautstärke 20 Prozent.")
        self.assertEqual(self.ask("VolumeDown"), "Lautstärke 10 Prozent.")

    def test_calculate(self):
        self.assertEqual(self.ask("Calculate", a=12, op="mal", b=7), "12 mal 7 ist 84.")
        self.assertEqual(self.ask("Calculate", a=3, op="minus", b=10), "3 minus 10 ist minus 7.")
        self.assertEqual(self.ask("Calculate", a=100, op="geteilt durch", b=8), "100 geteilt durch 8 ist 12,5.")
        self.assertEqual(self.ask("Calculate", a=10, op="dividiert durch", b=3), "10 dividiert durch 3 ist 3,33.")
        self.assertEqual(self.ask("Calculate", a=1, op="geteilt durch", b=0), "Durch null kann ich nicht teilen.")

    def test_repeat_and_unknown(self):
        self.assertEqual(self.ask("Repeat"), "Ich habe noch nichts gesagt.")
        self.ask("Thanks")
        self.assertEqual(self.ask("Repeat"), "Gern geschehen.")
        self.assertEqual(self.ask("Repeat"), "Gern geschehen.")
        self.assertEqual(self.ask("Nope"), "Dafür habe ich noch keine Antwort.")

    def test_number_words(self):
        n = ih.german_numbers()
        self.assertEqual((n["null"], n["eins"], n["zwölf"], n["einundzwanzig"], n["dreißig"],
                          n["siebenundneunzig"], n["hundert"], n["einhundert"]), (0, 1, 12, 21, 30, 97, 100, 100))
        self.assertEqual(sorted(set(n.values())), list(range(101)))
        self.assertEqual(self.ask("SetTimer", amount="dreißig", unit="sekunden"), "Timer auf 30 Sekunden gestellt.")
        self.assertEqual(self.ask("Calculate", a="zwölf", op="mal", b="sieben"), "12 mal 7 ist 84.")
        self.assertEqual(self.ask("SetVolume", volume="siebzig"), "Lautstärke 70 Prozent.")

    def test_rhasspy_slot_layout(self):
        e = {"intent": {"intentName": "SetVolume"}, "siteId": "x", "slots": [{"entity": "volume", "value": 70}]}
        self.assertEqual(self.a.handle(e), "Lautstärke 70 Prozent.")


if __name__ == "__main__":
    unittest.main()
