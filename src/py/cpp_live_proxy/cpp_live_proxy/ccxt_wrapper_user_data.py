class CcxtWrapperUserDataMixin:
    def register_listener(self, queue):
        self.listeners.add(queue)

    def unregister_listener(self, queue):
        self.listeners.discard(queue)

    async def broadcast(self, message):
        for queue in list(self.listeners):
            try:
                await queue.put(message)
            except Exception:
                self.listeners.discard(queue)

    async def disconnect_all_user_data_listeners(self) -> int:
        count = 0
        for queue in list(self.listeners):
            try:
                await queue.put(None)
                count += 1
            except Exception:
                self.listeners.discard(queue)
        return count
