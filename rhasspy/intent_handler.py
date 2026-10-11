#!/usr/bin/env python3
"""Hermes intent handler for the AI-VOX3 Rhasspy setup.

Answers recognized intents by publishing hermes/dialogueManager/endSession
with the answer text: the dialogue manager speaks it on the satellite and
ends the session (Rhasspy's "command" handler only speaks and leaves the
session open until it times out). Runs in the Rhasspy image, which ships
paho-mqtt; see docker-compose.yml.
"""
import datetime
import json
import logging
import os

import paho.mqtt.client as mqtt

WEEKDAYS = ["Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag", "Sonntag"]
MONTHS = ["Januar", "Februar", "März", "April", "Mai", "Juni", "Juli",
          "August", "September", "Oktober", "November", "Dezember"]


def answer(intent: str, now: datetime.datetime) -> str:
    if intent == "GetTime":
        return f"Es ist {now.hour} Uhr {now.minute}." if now.minute else f"Es ist {now.hour} Uhr."
    if intent == "GetDate":
        return f"Heute ist {WEEKDAYS[now.weekday()]}, der {now.day}. {MONTHS[now.month - 1]}."
    if intent == "Greeting":
        return "Hallo! Was kann ich für dich tun?"
    return "Dafür habe ich noch keine Antwort."


def on_connect(client, userdata, flags, rc):
    logging.info("connected to MQTT (rc=%s)", rc)
    client.subscribe("hermes/intent/#")


def on_message(client, userdata, msg):
    try:
        event = json.loads(msg.payload)
    except ValueError:
        return
    session_id = event.get("sessionId")
    intent = (event.get("intent") or {}).get("intentName", "")
    if not session_id:
        return
    text = answer(intent, datetime.datetime.now())
    logging.info("%s (%s): %s", intent, event.get("siteId"), text)
    client.publish("hermes/dialogueManager/endSession",
                   json.dumps({"sessionId": session_id, "text": text}, ensure_ascii=False))


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(os.environ.get("MQTT_HOST", "localhost"), int(os.environ.get("MQTT_PORT", "1883")))
    client.loop_forever()


if __name__ == "__main__":
    main()
