import json

from fastapi import FastAPI
from fastapi.testclient import TestClient

import cpp_live_proxy.api.v1.user_data as user_data


def test_idle_stream_sends_heartbeats_then_data(monkeypatch):
    monkeypatch.setattr(user_data, "HEARTBEAT_INTERVAL_S", 0.05)
    app = FastAPI()
    app.include_router(user_data.router)
    wrapper = user_data.CcxtWrapper()
    with TestClient(app).websocket_connect("/stream") as ws:
        assert json.loads(ws.receive_text()) == {"event_type": "heartbeat"}
        queue = next(iter(wrapper.listeners))
        queue.put_nowait({"event_type": "order_update", "orders": []})
        msg = json.loads(ws.receive_text())
        while msg["event_type"] == "heartbeat":
            msg = json.loads(ws.receive_text())
        assert msg == {"event_type": "order_update", "orders": []}
