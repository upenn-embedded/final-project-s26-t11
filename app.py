from flask import Flask, render_template, jsonify
import requests

app = Flask(__name__)

ESP32_URL = "http://172.20.10.3"

led_state = False


@app.route('/')
def index():
    return render_template('index.html', led_on=led_state)


@app.route('/toggle_led', methods=['POST'])
def toggle_led():
    global led_state
    endpoint = '/led/on' if not led_state else '/led/off'
    try:
        r = requests.post(f"{ESP32_URL}{endpoint}", timeout=3)
        r.raise_for_status()
        led_state = not led_state
        return jsonify({'success': True, 'led_on': led_state})
    except requests.RequestException as e:
        return jsonify({'success': False, 'error': str(e)}), 502


if __name__ == '__main__':
    app.run(host='0.0.0.0', debug=True)
