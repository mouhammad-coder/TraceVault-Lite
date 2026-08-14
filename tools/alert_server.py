"""
TraceVault Lite - optional Flask alert-server prototype.

The server receives security events from the ESP32, writes valid events to a
CSV file, and sends Telegram notifications. It never receives or stores a PIN.

The current TraceVault_Lite.ino firmware sends Telegram alerts directly and
does not call this server. This file is preserved as an optional extension.
"""

from flask import Flask, jsonify, request
import requests
import csv
import os
from datetime import datetime, timezone


# Create the Flask application and limit incoming requests to 1 KB.
app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 1024

# The CSV is stored beside server.py.
base_directory = os.path.dirname(os.path.abspath(__file__))
csv_file_path = os.path.join(base_directory, "tracevault_events.csv")

# Only these event names are accepted from the ESP32.
known_events = [
    "UNAUTHORIZED_RFID",
    "WRONG_PIN",
    "LOCKDOWN",
    "ACCESS_GRANTED",
    "SYSTEM_STARTED",
]


def get_server_time():
    """Return a clear UTC date and time string."""
    current_time = datetime.now(timezone.utc)
    return current_time.strftime("%Y-%m-%d %H:%M:%S UTC")


def save_event_to_csv(timestamp, event, attempt, door_status, device):
    """Append one validated event and create the header when needed."""
    file_exists = os.path.exists(csv_file_path)

    with open(csv_file_path, "a", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)

        if file_exists == False or os.path.getsize(csv_file_path) == 0:
            writer.writerow([
                "timestamp",
                "event",
                "attempt",
                "door_status",
                "device",
            ])

        writer.writerow([timestamp, event, attempt, door_status, device])


def build_telegram_message(event, attempt, door_status, timestamp):
    """Create a short Telegram message for each known event."""
    if event == "WRONG_PIN":
        message = (
            "TRACEVAULT SECURITY ALERT\n"
            "Incorrect PIN entered\n"
            f"Attempt: {attempt} of 3\n"
            f"Door status: {door_status}\n"
            f"Time: {timestamp}"
        )
    elif event == "UNAUTHORIZED_RFID":
        message = (
            "TRACEVAULT SECURITY ALERT\n"
            "Unauthorized RFID detected\n"
            f"Attempt: {attempt} of 3\n"
            f"Door status: {door_status}\n"
            f"Time: {timestamp}"
        )
    elif event == "LOCKDOWN":
        message = (
            "TRACEVAULT CRITICAL ALERT\n"
            "Multiple failed access attempts detected\n"
            "The safe entered lockdown mode\n"
            f"Door status: {door_status}\n"
            f"Time: {timestamp}"
        )
    elif event == "ACCESS_GRANTED":
        message = (
            "TRACEVAULT ACCESS NOTICE\n"
            "Authorized access granted\n"
            f"Door status: {door_status}\n"
            f"Time: {timestamp}"
        )
    else:
        message = (
            "TRACEVAULT SYSTEM NOTICE\n"
            "Security system started\n"
            f"Door status: {door_status}\n"
            f"Time: {timestamp}"
        )

    # Telegram permits longer text, but this project intentionally stays small.
    return message[:500]


def send_telegram_message(message):
    """Send one Telegram message and fail safely when Telegram is offline."""
    bot_token = os.environ.get("TELEGRAM_BOT_TOKEN", "")
    chat_id = os.environ.get("TELEGRAM_CHAT_ID", "")

    if bot_token == "" or chat_id == "":
        print("Telegram message not sent: environment variables are missing.")
        return False

    telegram_url = f"https://api.telegram.org/bot{bot_token}/sendMessage"
    telegram_data = {
        "chat_id": chat_id,
        "text": message,
    }

    try:
        response = requests.post(
            telegram_url,
            data=telegram_data,
            timeout=5,
        )

        if response.status_code == 200:
            return True

        print("Telegram returned an error status.")
        return False
    except requests.RequestException:
        # Do not print the URL because it contains the secret bot token.
        print("Telegram connection failed. Event was still saved to CSV.")
        return False


def validate_alert_data(data):
    """Validate only the four fields used by TraceVault."""
    if isinstance(data, dict) == False:
        return "Request body must be a JSON object."

    # Explicitly reject common secret field names. Unexpected non-secret fields
    # are ignored and are never written to the CSV file.
    forbidden_fields = ["pin", "correct_pin", "password", "wifi_password"]
    for received_field_name in data:
        if received_field_name.lower() in forbidden_fields:
            return "PIN or password fields are not accepted."

    event = data.get("event")
    attempt = data.get("attempt")
    door_status = data.get("door_status")
    device = data.get("device")

    if isinstance(event, str) == False or event not in known_events:
        return "Unknown event."

    # bool is a type of int in Python, so reject it explicitly.
    if isinstance(attempt, bool) == True or isinstance(attempt, int) == False:
        return "Attempt must be an integer."

    if attempt < 0 or attempt > 3:
        return "Attempt must be between 0 and 3."

    if door_status not in ["LOCKED", "UNLOCKED"]:
        return "Invalid door status."

    if device != "TRACEVAULT":
        return "Invalid device."

    # Enforce the expected relationships between events and the door state.
    if event == "ACCESS_GRANTED" and door_status != "UNLOCKED":
        return "ACCESS_GRANTED requires an unlocked door status."

    if event != "ACCESS_GRANTED" and door_status != "LOCKED":
        return "This event requires a locked door status."

    return ""


@app.route("/health", methods=["GET"])
def health():
    """Simple endpoint used to check whether the service is online."""
    return jsonify({
        "status": "online",
        "project": "TraceVault",
    }), 200


@app.route("/alert", methods=["POST"])
def receive_alert():
    """Authenticate, validate, save, and notify for one ESP32 event."""
    expected_api_key = os.environ.get("TRACEVAULT_API_KEY", "")
    provided_api_key = request.headers.get("X-API-Key", "")

    if expected_api_key == "":
        return jsonify({
            "success": False,
            "message": "Server API key is not configured.",
        }), 500

    if provided_api_key == "" or provided_api_key != expected_api_key:
        return jsonify({
            "success": False,
            "message": "Unauthorized request.",
        }), 401

    if request.is_json == False:
        return jsonify({
            "success": False,
            "message": "Content-Type must be application/json.",
        }), 415

    data = request.get_json(silent=True)
    validation_error = validate_alert_data(data)

    if validation_error != "":
        return jsonify({
            "success": False,
            "message": validation_error,
        }), 400

    # Read only the expected fields. Any other fields are ignored.
    event = data.get("event")
    attempt = data.get("attempt")
    door_status = data.get("door_status")
    device = data.get("device")
    timestamp = get_server_time()

    try:
        save_event_to_csv(timestamp, event, attempt, door_status, device)
    except (OSError, csv.Error):
        return jsonify({
            "success": False,
            "message": "The valid event could not be saved.",
        }), 500

    message = build_telegram_message(
        event,
        attempt,
        door_status,
        timestamp,
    )
    telegram_sent = send_telegram_message(message)

    return jsonify({
        "success": True,
        "message": "Event accepted and saved.",
        "telegram_sent": telegram_sent,
    }), 200


# Run this development server when testing server.py directly.
if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
