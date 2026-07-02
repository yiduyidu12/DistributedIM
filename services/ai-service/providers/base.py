# AI Provider 抽象基类
from abc import ABC, abstractmethod

class BaseAIProvider(ABC):
    def __init__(self, name, api_key, model):
        self.name = name
        self.api_key = api_key
        self.model = model

    @abstractmethod
    async def chat(self, messages, **kwargs):
        ...

    @abstractmethod
    async def check_health(self):
        ...

    async def chat_stream(self, messages, **kwargs):
        raise NotImplementedError
