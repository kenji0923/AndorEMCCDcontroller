"""Bridge BLACS's science_starting callback to the camera worker.

Registered by the tab before transitioning. Tokens are unique per attempt so
an aborted/repeated shot cannot consume an old start notification. The event
is only a software time origin; it is not a hardware pseudoclock edge.
"""

import os
import threading
import uuid

EVENT_NAME = "andor_emccd_science_starting"
PLUGIN_KEY = "andor_emccd_software_trigger"


class ShotStartBridge:
    def __init__(self):
        from labscript_utils.ls_zprocess import Event
        self.event = Event(EVENT_NAME, role="post")
        self.pending = {}
        self.lock = threading.Lock()

    def register(self, device_name, path):
        token = uuid.uuid4().hex
        with self.lock:
            self.pending[device_name] = (os.path.abspath(path), token)
        return token

    def science_starting(self, path):
        with self.lock:
            selected = [(name, token) for name, (shot, token) in self.pending.items()
                        if shot == os.path.abspath(path)]
            for name, token in selected:
                del self.pending[name]
                self.event.post(token)

    # Run after cycle-time and other preparation callbacks.
    science_starting.priority = 1000

    def get_callbacks(self):
        return {"science_starting": self.science_starting}

    def get_save_data(self):
        return {}

    def close(self):
        with self.lock:
            self.pending.clear()


def register_shot(device_name, path):
    import __main__
    app = __main__.app
    if PLUGIN_KEY not in app.plugins:
        app.plugins[PLUGIN_KEY] = ShotStartBridge()
    return app.plugins[PLUGIN_KEY].register(device_name, path)
