from typing import Dict, List, Any


class BitgetUtils:
    @staticmethod
    def parse_raw_order_res(res: Dict, market_type: str) -> Any:
        if "data" not in res:
            raise Exception("data not found in res")

        data_dict = res["data"]

        if "data" in data_dict:
            data = data_dict["data"]
        else:
            data = data_dict

        if "orderList" not in data:
            raise Exception("orderList not found in res")

        order_list = data["orderList"]
        ret = []

        for item in order_list:
            order_item: Dict[str, Any] = {}
            order_item["symbol"] = str(item["symbol"])
            order_item["id"] = str(item["orderId"])
            order_item["clientOrderId"] = str(item["clientOid"])
            order_item["status"] = str(item["status"])
            order_item["exchange_name"] = "bitget"
            order_item["market_type"] = market_type

            order_item["price"] = float(item["price"])

            if "priceAvg" in item and float(item["priceAvg"]) > 0:
                order_item["average"] = float(item["priceAvg"])

            order_item["amount"] = float(item["baseSize"])
            order_item["filled"] = float(item["size"])

            order_item["timestamp"] = int(item["cTime"])
            order_item["lastUpdateTimestamp"] = int(item["uTime"])
            order_item["lastTradeTimestamp"] = int(item["uTime"])

            if float(item["amount"]) > 0:
                fee: Dict[str, Any] = {}
                fee["currency"] = "USDT"
                fee["cost"] = float(item["amount"]) * 0.001
                order_item["fee"] = fee

            if item["status"] == "live":
                order_item["status"] = "open"
            elif item["status"] in ["filled", "cancelled", "expired", "rejected"]:
                order_item["status"] = "closed"
            else:
                order_item["status"] = "closed"
            ret.append(order_item)

        return ret
