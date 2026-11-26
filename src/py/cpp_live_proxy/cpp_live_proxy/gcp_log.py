import os

# Off in test mode and on hosts without Google credentials (the market data
# server); log() then prints.
EMIT_GCP_LOGS = (
    os.getenv("EMIT_GCP_LOGS", "0") == "1"
    and os.getenv("CPP_LIVE_PROXY_TEST_MODE", "0") != "1"
)

gcp_logger = None
if EMIT_GCP_LOGS:
    from google.cloud import logging

    gcp_logger = logging.Client().logger("ccxp_live_proxy")


def log(message: str, metadata: dict, severity: str = "INFO"):
    if gcp_logger is None:
        print(message, "\n", metadata)
        return
    entry = {"message": message, "metadata": metadata}
    gcp_logger.log_struct(entry, severity=severity)
