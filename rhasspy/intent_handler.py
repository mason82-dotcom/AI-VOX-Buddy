#!/usr/bin/env python3
"""Hermes intent handler for the AI-VOX3 Rhasspy setup.

Answers recognized intents by publishing hermes/dialogueManager/endSession
with the answer text: the dialogue manager speaks it on the satellite and
ends the session (Rhasspy's "command" handler only speaks and leaves the
session open until it times out). Runs in the Rhasspy image, which ships
paho-mqtt; see docker-compose.yml. Tests: python3 rhasspy/test_intent_handler.py
"""
import datetime
import json
import logging
import os
import threading
import typing

WEEKDAYS = ["Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag", "Sonntag"]
MONTHS = ["Januar", "Februar", "März", "April", "Mai", "Juni", "Juli",
          "August", "September", "Oktober", "November", "Dezember"]
VOLUME_STEP, VOLUME_MIN, DEFAULT_VOLUME = 15, 10, 40
HELP = ("Ich kann dir Uhrzeit und Datum sagen, einen Timer stellen, die Lautstärke ändern, "
        "einfache Aufgaben rechnen und sagen, wie lange es noch bis Weihnachten ist.")

Publish = typing.Callable[[str, dict], None]

_UNITS = ["null", "eins", "zwei", "drei", "vier", "fünf", "sechs", "sieben", "acht", "neun",
          "zehn", "elf", "zwölf", "dreizehn", "vierzehn", "fünfzehn", "sechzehn", "siebzehn",
          "achtzehn", "neunzehn"]
_TENS = {20: "zwanzig", 30: "dreißig", 40: "vierzig", 50: "fünfzig", 60: "sechzig",
         70: "siebzig", 80: "achtzig", 90: "neunzig"}


def german_numbers() -> typing.Dict[str, int]:
    """German number words 0..100 (as Vosk transcribes them) mapped to their values.

    Rhasspy's number ranges like (1..60) are unusable with Vosk: its training
    puts the substituted digits instead of the spoken words into the Vosk
    vocabulary, so number words can never be recognized. The sentences use the
    plain-word slot list profile/slots/zahl (rhasspy/gen-slots.py) instead.
    """
    words = {w: i for i, w in enumerate(_UNITS)}
    for tens, name in _TENS.items():
        words[name] = tens
        for unit in range(1, 10):
            prefix = "ein" if unit == 1 else _UNITS[unit]
            words[f"{prefix}und{name}"] = tens + unit
    words.update({"ein": 1, "eine": 1, "einen": 1, "einer": 1, "hundert": 100, "einhundert": 100})
    return words


NUMBERS = german_numbers()


def slot_values(event: dict) -> typing.Dict[str, typing.Any]:
    """Maps slot names to values for both Hermes and Rhasspy slot layouts."""
    values = {}
    for slot in event.get("slots") or []:
        value = slot.get("value")
        values[slot.get("slotName") or slot.get("entity")] = (
            value.get("value") if isinstance(value, dict) else value)
    return values


def to_int(value: typing.Any) -> typing.Optional[int]:
    if isinstance(value, str) and value.strip().lower() in NUMBERS:
        return NUMBERS[value.strip().lower()]
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return None


def number_text(x: float) -> str:
    """Formats a result for German speech: integer, or up to two decimals with comma."""
    if abs(x - round(x)) < 1e-9:
        text = str(int(round(x)))
    else:
        text = f"{x:.2f}".rstrip("0").rstrip(".").replace(".", ",")
    return "minus " + text.lstrip("-") if text.startswith("-") else text


def duration_text(seconds: int) -> str:
    minutes, seconds = divmod(max(0, seconds), 60)
    parts = []
    if minutes:
        parts.append("eine Minute" if minutes == 1 else f"{minutes} Minuten")
    if seconds or not minutes:
        parts.append("eine Sekunde" if seconds == 1 else f"{seconds} Sekunden")
    return " und ".join(parts)


class Assistant:
    """Intent logic; MQTT-independent so it can be unit-tested."""

    def __init__(self, publish: Publish,
                 now: typing.Callable[[], datetime.datetime] = datetime.datetime.now,
                 timer_factory=threading.Timer):
        self.publish = publish
        self.now = now
        self.timer_factory = timer_factory
        self.timers: typing.Dict[str, typing.Tuple[typing.Any, datetime.datetime]] = {}
        self.last_answer: typing.Dict[str, str] = {}
        self.volume: typing.Dict[str, int] = {}
        self.lock = threading.Lock()

    def on_volume(self, site_id: str, volume: int) -> None:
        self.volume[site_id] = volume

    def handle(self, event: dict) -> str:
        intent = (event.get("intent") or {}).get("intentName", "")
        site = event.get("siteId") or "default"
        slots = slot_values(event)
        method = getattr(self, "intent_" + intent, None)
        text = method(site, slots) if method else "Dafür habe ich noch keine Antwort."
        if intent != "Repeat":
            self.last_answer[site] = text
        return text

    # Time and date ----------------------------------------------------------
    def intent_GetTime(self, site, slots):
        now = self.now()
        return f"Es ist {now.hour} Uhr {now.minute}." if now.minute else f"Es ist {now.hour} Uhr."

    def intent_GetDate(self, site, slots):
        now = self.now()
        return f"Heute ist {WEEKDAYS[now.weekday()]}, der {now.day}. {MONTHS[now.month - 1]} {now.year}."

    def intent_DaysUntil(self, site, slots):
        today = self.now().date()
        event = str(slots.get("event", "weihnachten")).lower()
        month, day, name = {"silvester": (12, 31, "Silvester"),
                            "neujahr": (1, 1, "Neujahr")}.get(event, (12, 24, "Weihnachten"))
        target = datetime.date(today.year, month, day)
        if target < today:
            target = target.replace(year=today.year + 1)
        days = (target - today).days
        if days == 0:
            return f"Heute ist {name}!"
        return f"Bis {name} ist es noch ein Tag." if days == 1 else f"Bis {name} sind es noch {days} Tage."

    # Timer ------------------------------------------------------------------
    def intent_SetTimer(self, site, slots):
        amount = to_int(slots.get("amount"))
        unit = str(slots.get("unit", "minuten")).lower()
        if not amount or amount <= 0:
            return "Wie lange soll der Timer laufen?"
        seconds = amount if unit.startswith("sekunde") else amount * 60
        with self.lock:
            self._cancel(site)
            timer = self.timer_factory(seconds, self._timer_done, args=(site,))
            timer.daemon = True
            self.timers[site] = (timer, self.now() + datetime.timedelta(seconds=seconds))
            timer.start()
        return f"Timer auf {duration_text(seconds)} gestellt."

    def intent_TimerStatus(self, site, slots):
        with self.lock:
            entry = self.timers.get(site)
        if not entry:
            return "Es läuft gerade kein Timer."
        remaining = int((entry[1] - self.now()).total_seconds() + 0.5)
        return f"Der Timer läuft noch {duration_text(remaining)}."

    def intent_CancelTimer(self, site, slots):
        with self.lock:
            cancelled = self._cancel(site)
        return "Timer abgebrochen." if cancelled else "Es läuft gerade kein Timer."

    def _cancel(self, site) -> bool:
        entry = self.timers.pop(site, None)
        if entry:
            entry[0].cancel()
        return entry is not None

    def _timer_done(self, site):
        with self.lock:
            self.timers.pop(site, None)
        logging.info("timer done (%s)", site)
        self.publish("hermes/dialogueManager/startSession", {
            "siteId": site,
            "init": {"type": "notification", "text": "Der Timer ist abgelaufen."},
        })

    # Volume (the satellite applies rhasspy/audioServer/setVolume) ------------
    def _set_volume(self, site, volume):
        volume = max(VOLUME_MIN, min(100, volume))
        self.volume[site] = volume
        self.publish("rhasspy/audioServer/setVolume", {"siteId": site, "volume": volume / 100})
        return f"Lautstärke {volume} Prozent."

    def intent_SetVolume(self, site, slots):
        volume = to_int(slots.get("volume"))
        return self._set_volume(site, volume) if volume is not None else "Welche Lautstärke?"

    def intent_VolumeUp(self, site, slots):
        return self._set_volume(site, self.volume.get(site, DEFAULT_VOLUME) + VOLUME_STEP)

    def intent_VolumeDown(self, site, slots):
        return self._set_volume(site, self.volume.get(site, DEFAULT_VOLUME) - VOLUME_STEP)

    # Arithmetic -------------------------------------------------------------
    def intent_Calculate(self, site, slots):
        a, b = to_int(slots.get("a")), to_int(slots.get("b"))
        op = str(slots.get("op", "")).lower()
        if a is None or b is None:
            return "Das konnte ich nicht rechnen."
        if op == "plus":
            result = a + b
        elif op == "minus":
            result = a - b
        elif op == "mal":
            result = a * b
        elif op.endswith("durch"):
            if b == 0:
                return "Durch null kann ich nicht teilen."
            result = a / b
        else:
            return "Das konnte ich nicht rechnen."
        return f"{a} {op} {b} ist {number_text(result)}."

    # Conversation -----------------------------------------------------------
    def intent_Greeting(self, site, slots):
        return "Hallo! Was kann ich für dich tun?"

    def intent_Thanks(self, site, slots):
        return "Gern geschehen."

    def intent_Help(self, site, slots):
        return HELP

    def intent_Identity(self, site, slots):
        return "Ich bin dein AI-VOX Buddy, ein Sprachassistent mit Rhasspy."

    def intent_Repeat(self, site, slots):
        return self.last_answer.get(site, "Ich habe noch nichts gesagt.")


def main() -> None:
    import paho.mqtt.client as mqtt

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    client = mqtt.Client()

    def publish(topic: str, payload: dict) -> None:
        client.publish(topic, json.dumps(payload, ensure_ascii=False))

    assistant = Assistant(publish)

    def on_connect(client, userdata, flags, rc):
        logging.info("connected to MQTT (rc=%s)", rc)
        client.subscribe("hermes/intent/#")
        client.subscribe("aivox3/+/volume")

    def on_message(client, userdata, msg):
        try:
            event = json.loads(msg.payload)
        except ValueError:
            return
        if msg.topic.startswith("aivox3/"):
            if isinstance(event.get("volume"), (int, float)):
                assistant.on_volume(event.get("siteId") or msg.topic.split("/")[1], int(event["volume"]))
            return
        session_id = event.get("sessionId")
        if not session_id:
            return
        text = assistant.handle(event)
        logging.info("%s (%s): %s", (event.get("intent") or {}).get("intentName"), event.get("siteId"), text)
        publish("hermes/dialogueManager/endSession", {"sessionId": session_id, "text": text})

    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(os.environ.get("MQTT_HOST", "localhost"), int(os.environ.get("MQTT_PORT", "1883")))
    client.loop_forever()


if __name__ == "__main__":
    main()
