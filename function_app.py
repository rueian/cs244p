import azure.functions as func
import logging
import json
import os
import threading
from collections import deque
from datetime import datetime
from azure.communication.email import EmailClient

# ================= GLOBAL STATE =================
# Note: In a Consumption plan, this state is cleared if the app scales to zero.
# It works best on an App Service Plan (Always On) or with frequent polling.

history = deque(maxlen=100)
mute_next_response = False

# Track previous states for edge detection
last_motion_state = False       
last_vault_status = "CLOSED"

# LOCK for thread safety (Python Functions use a thread pool for sync requests)
data_lock = threading.Lock()

app = func.FunctionApp(http_auth_level=func.AuthLevel.ANONYMOUS)

# ================= HELPER: SEND EMAIL (ACS) =================
def send_acs_email(subject, body):
    """Sends an email using Azure Communication Services."""
    connection_string = os.environ.get("COMMUNICATION_CONNECTION_STRING")
    sender_address = os.environ.get("SENDER_ADDRESS")
    recipient_address = os.environ.get("RECIPIENT_ADDRESS")

    if not connection_string or not sender_address or not recipient_address:
        logging.error("Missing Azure Communication Services configuration.")
        return

    try:
        client = EmailClient.from_connection_string(connection_string)

        message = {
            "senderAddress": sender_address,
            "recipients":  {
                "to": [{"address": recipient_address}]
            },
            "content": {
                "subject": f"[VaultAlert] {subject}",
                "plainText": body
            }
        }

        poller = client.begin_send(message)
        result = poller.result()
        logging.info(f"Email sent successfully. Message ID: {result['messageId']}")
        
    except Exception as e:
        logging.error(f"Failed to send email via ACS: {e}")

# ================= ENDPOINT 1: COLLECT =================
@app.route(route="collect", auth_level=func.AuthLevel.ANONYMOUS)
def collect_data(req: func.HttpRequest) -> func.HttpResponse:
    global mute_next_response, last_motion_state, last_vault_status

    try:
        # 1. Parse Data
        req_body = req.get_json()
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        
        # Local variables to determine actions AFTER releasing the lock
        should_send_email = False
        email_triggers = []
        respond_with_mute = False
        
        # 2. CRITICAL SECTION: Access and modify shared state
        with data_lock:
            # Store Data
            record = {
                "timestamp": timestamp,
                "data": req_body
            }
            history.append(record)
            
            # Logic Extraction
            current_motion = req_body.get("motion_detected", False)
            current_status = req_body.get("vault_status", "CLOSED")
            current_light = req_body.get("light_level", 0)

            # Edge Detection Logic
            # Trigger if motion goes False -> True
            if current_motion and not last_motion_state:
                email_triggers.append("Motion Detected")

            # Trigger if status goes CLOSED -> OPEN
            if current_status == "OPEN" and last_vault_status == "CLOSED":
                email_triggers.append("Vault Opened")

            # Update state for next comparison
            last_motion_state = current_motion
            last_vault_status = current_status
            
            # Decide if we need to email
            if email_triggers:
                should_send_email = True

            # Handle Response (Mute Logic)
            # We check and reset the flag inside the lock to be atomic
            if mute_next_response:
                respond_with_mute = True
                mute_next_response = False  # Reset flag immediately

        # 3. NON-CRITICAL SECTION: Network I/O (Email)
        # We perform this OUTSIDE the lock so we don't block other requests
        if should_send_email:
            subject = " | ".join(email_triggers)
            body = (
                f"Security Alert Triggered!\n\n"
                f"Events: {', '.join(email_triggers)}\n"
                f"Time: {timestamp}\n"
                f"Light Level: {current_light}\n"
                f"Status: {current_status}\n"
            )
            logging.info(f"Trigger detected: {subject}. Sending email...")
            send_acs_email(subject, body)

        # 4. Return Response
        if respond_with_mute:
            logging.info("Sending MUTE command to device.")
            return func.HttpResponse("false", status_code=200)
        
        return func.HttpResponse("true", status_code=200)

    except ValueError:
        return func.HttpResponse("Invalid JSON", status_code=400)
    except Exception as e:
        logging.error(f"Error in collect: {e}")
        return func.HttpResponse(f"Server Error: {e}", status_code=500)

# ================= ENDPOINT 2: MUTE =================
@app.route(route="mute", auth_level=func.AuthLevel.ANONYMOUS)
def mute_alarm(req: func.HttpRequest) -> func.HttpResponse:
    global mute_next_response
    
    with data_lock:
        mute_next_response = True
        
    logging.info("Mute requested manually.")
    
    return func.HttpResponse(
        "Mute command queued. The alarm will stop the next time the device reports in.",
        status_code=200
    )

# ================= ENDPOINT 3: PLOT =================
@app.route(route="plot", auth_level=func.AuthLevel.ANONYMOUS)
def plot_data(req: func.HttpRequest) -> func.HttpResponse:
    with data_lock:
        data_snapshot = list(history)

    # Extract time series
    timestamps = []
    statuses = []   # OPEN=1, CLOSED=0
    motions = []    # True=1, False=0

    for record in data_snapshot:
        timestamps.append(record["timestamp"])

        data = record["data"]
        state = data.get("vault_status", "CLOSED")
        motion = data.get("motion_detected", False)

        statuses.append(1 if state == "OPEN" else 0)
        motions.append(1 if motion else 0)

    # Build HTML
    html = f"""
    <!DOCTYPE html>
    <html>
    <head>
        <title>VaultAlert History Plot</title>
        <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
        <style>
            body {{
                font-family: Arial, sans-serif;
                padding: 20px;
                background: #f6f6f6;
            }}
            #chart-container {{
                width: 95%;
                max-width: 850px;
                margin: auto;
                background: white;
                padding: 20px;
                border-radius: 12px;
                box-shadow: 0 3px 10px rgba(0,0,0,0.15);
            }}
        </style>
    </head>
    <body>
        <h2>Vault Status & Motion History</h2>
        <p>OPEN=1, CLOSED=0 | Motion: YES=1, NO=0</p>

        <div id="chart-container">
            <canvas id="vaultChart"></canvas>
        </div>

        <script>
            const labels = {json.dumps(timestamps)};
            const vaultStatus = {json.dumps(statuses)};
            const motionData = {json.dumps(motions)};

            const ctx = document.getElementById('vaultChart').getContext('2d');

            new Chart(ctx, {{
                type: 'line',
                data: {{
                    labels: labels,
                    datasets: [
                        {{
                            label: 'Vault Status (1=OPEN, 0=CLOSED)',
                            data: vaultStatus,
                            borderWidth: 2,
                            borderColor: 'rgb(255, 99, 132)',
                            backgroundColor: 'rgba(255, 99, 132, 0.3)',
                            fill: false,
                            tension: 0.2
                        }},
                        {{
                            label: 'Motion Detected (1=YES, 0=NO)',
                            data: motionData,
                            borderWidth: 2,
                            borderColor: 'rgb(54, 162, 235)',
                            backgroundColor: 'rgba(54, 162, 235, 0.3)',
                            fill: false,
                            tension: 0.2
                        }}
                    ]
                }},
                options: {{
                    scales: {{
                        y: {{
                            min: 0,
                            max: 1,
                            ticks: {{
                                callback: (v) => v === 1 ? "1" : "0"
                            }}
                        }},
                        x: {{
                            ticks: {{
                                maxRotation: 45,
                                minRotation: 45
                            }}
                        }}
                    }}
                }}
            }});
        </script>
    </body>
    </html>
    """

    return func.HttpResponse(html, mimetype="text/html")
