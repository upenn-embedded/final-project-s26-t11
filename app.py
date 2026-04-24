from flask import Flask, render_template, jsonify
import requests

app = Flask(__name__)

ESP32_URL = "http://172.20.10.3"


@app.route('/')
def index():
    try:
        r = requests.get(f"{ESP32_URL}/gimbal/state", timeout=3)
        r.raise_for_status()
        gimbal_enabled = r.json().get('gimbal_enabled', False)
    except requests.RequestException:
        gimbal_enabled = False
    return render_template('index.html', gimbal_enabled=gimbal_enabled)


@app.route('/toggle_gimbal', methods=['POST'])
def toggle_gimbal():
    try:
        r = requests.post(f"{ESP32_URL}/gimbal/toggle", timeout=3)
        r.raise_for_status()
        gimbal_enabled = r.json().get('gimbal_enabled', False)
        return jsonify({'success': True, 'gimbal_enabled': gimbal_enabled})
    except requests.RequestException as e:
        return jsonify({'success': False, 'error': str(e)}), 502


@app.route('/state', methods=['GET'])
def state():
    try:
        r = requests.get(f"{ESP32_URL}/gimbal/state", timeout=3)
        r.raise_for_status()
        gimbal_enabled = r.json().get('gimbal_enabled', False)
        return jsonify({'success': True, 'gimbal_enabled': gimbal_enabled})
    except requests.RequestException as e:
        return jsonify({'success': False, 'error': str(e)}), 502


if __name__ == '__main__':
    app.run(host='0.0.0.0', debug=True)
