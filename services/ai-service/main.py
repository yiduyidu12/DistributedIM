"""
DistributedIM - AI 服务 (Python/FastAPI)
统一 AI 抽象层：支持 Claude、DeepSeek、通义千问自动故障转移
提供聊天机器人、离线摘要、情感分析功能
"""

import os
import time
import json
import hashlib
from typing import Optional, AsyncGenerator
from abc import ABC, abstractmethod

import redis.asyncio as redis
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
import httpx

app = FastAPI(title="DistributedIM AI Service")

# Redis 连接
redis_client: Optional[redis.Redis] = None

# ============ 配置 ============

PROVIDERS = [
    {
        "name": "claude",
        "url": "https://api.anthropic.com/v1/messages",
        "api_key": os.getenv("ANTHROPIC_API_KEY", ""),
        "model": "claude-sonnet-4-6",
    },
    {
        "name": "deepseek",
        "url": "https://api.deepseek.com/v1/chat/completions",
        "api_key": os.getenv("DEEPSEEK_API_KEY", ""),
        "model": "deepseek-chat",
    },
    {
        "name": "qwen",
        "url": "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
        "api_key": os.getenv("QWEN_API_KEY", ""),
        "model": "qwen-turbo",
    },
]

# ============ 抽象基类 ============

class AIProvider(ABC):
    """AI 提供商抽象基类"""

    @abstractmethod
    async def chat(self, messages: list[dict], **kwargs) -> str:
        """发送聊天请求"""
        ...

    @abstractmethod
    async def check_health(self) -> bool:
        """健康检查"""
        ...


class OpenAICompatibleProvider(AIProvider):
    """OpenAI 兼容 API 提供商适配器"""

    def __init__(self, name: str, url: str, api_key: str, model: str):
        self.name = name
        self.url = url
        self.api_key = api_key
        self.model = model

    async def chat(self, messages: list[dict], **kwargs) -> str:
        if not self.api_key:
            raise ValueError(f"Provider {self.name} not configured")

        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }

        body = {
            "model": self.model,
            "messages": messages,
            "max_tokens": kwargs.get("max_tokens", 1024),
            "temperature": kwargs.get("temperature", 0.7),
        }

        async with httpx.AsyncClient(timeout=60.0) as client:
            resp = await client.post(self.url, headers=headers, json=body)
            resp.raise_for_status()
            data = resp.json()
            return data["choices"][0]["message"]["content"]

    async def check_health(self) -> bool:
        if not self.api_key:
            return False
        try:
            async with httpx.AsyncClient(timeout=5.0) as client:
                resp = await client.get(
                    self.url.replace("/chat/completions", "/models"),
                    headers={"Authorization": f"Bearer {self.api_key}"},
                )
                return resp.status_code == 200
        except Exception:
            return False


class ClaudeProvider(AIProvider):
    """Claude API 适配器"""

    def __init__(self, name: str, url: str, api_key: str, model: str):
        self.name = name
        self.url = url
        self.api_key = api_key
        self.model = model

    async def chat(self, messages: list[dict], **kwargs) -> str:
        if not self.api_key:
            raise ValueError(f"Provider {self.name} not configured")

        headers = {
            "x-api-key": self.api_key,
            "anthropic-version": "2023-06-01",
            "Content-Type": "application/json",
        }

        # 转换消息格式到 Claude API 格式
        system_msg = ""
        claude_messages = []
        for msg in messages:
            if msg["role"] == "system":
                system_msg = msg["content"]
            else:
                claude_messages.append({"role": msg["role"], "content": msg["content"]})

        body = {
            "model": self.model,
            "max_tokens": kwargs.get("max_tokens", 1024),
            "messages": claude_messages,
        }
        if system_msg:
            body["system"] = system_msg

        async with httpx.AsyncClient(timeout=60.0) as client:
            resp = await client.post(self.url, headers=headers, json=body)
            resp.raise_for_status()
            data = resp.json()
            return data["content"][0]["text"]

    async def check_health(self) -> bool:
        if not self.api_key:
            return False
        return True


# ============ 统一 AI 管理器 ============

class AIManager:
    """统一 AI 抽象层，支持自动故障转移"""

    def __init__(self):
        self.providers: list[AIProvider] = []
        for p in PROVIDERS:
            if p["name"] == "claude":
                self.providers.append(
                    ClaudeProvider(p["name"], p["url"], p["api_key"], p["model"])
                )
            else:
                self.providers.append(
                    OpenAICompatibleProvider(p["name"], p["url"], p["api_key"], p["model"])
                )

    async def chat_with_failover(self, messages: list[dict], **kwargs) -> str:
        """聊天请求，自动故障转移到下一个可用 Provider"""
        last_error = None
        for provider in self.providers:
            try:
                start = time.time()
                result = await provider.chat(messages, **kwargs)
                elapsed = (time.time() - start) * 1000
                # 记录成功指标（简化）
                print(f"[AI] {provider.name} succeeded in {elapsed:.0f}ms")
                return result
            except Exception as e:
                print(f"[AI] {provider.name} failed: {e}")
                last_error = e
                continue
        raise HTTPException(status_code=503, detail=f"All AI providers failed: {last_error}")

    async def get_health(self) -> dict[str, bool]:
        """获取所有 Provider 的健康状态"""
        health = {}
        for provider in self.providers:
            health[provider.name] = await provider.check_health()
        return health


ai_manager = AIManager()


# ============ API 模型 ============

class ChatRequest(BaseModel):
    prompt: str
    context: list[dict] = []  # 对话历史
    max_tokens: int = 1024
    temperature: float = 0.7


class SummarizeRequest(BaseModel):
    messages: str  # JSON 格式的消息列表


class SentimentRequest(BaseModel):
    text: str


class ChatResponse(BaseModel):
    provider: str
    content: str
    usage: dict = {}


# ============ 启动/关闭事件 ============

@app.on_event("startup")
async def startup():
    global redis_client
    redis_client = redis.Redis(
        host=os.getenv("REDIS_HOST", "localhost"),
        port=int(os.getenv("REDIS_PORT", "6379")),
        decode_responses=True,
    )


@app.on_event("shutdown")
async def shutdown():
    if redis_client:
        await redis_client.close()


# ============ API 路由 ============

@app.get("/health")
async def health():
    """健康检查 + Provider 状态"""
    provider_health = await ai_manager.get_health()
    return {"status": "ok", "providers": provider_health}


@app.post("/ai/chat", response_model=ChatResponse)
async def chat(req: ChatRequest):
    """AI 聊天（群聊 @bot 或 !ai 触发）"""
    messages = []
    # 添加上下文
    if req.context:
        messages.extend(req.context)

    # 添加系统提示
    messages.insert(0, {
        "role": "system",
        "content": "你是一个友好的聊天助手，请用简洁的中文回答。",
    })

    # 添加用户消息
    messages.append({"role": "user", "content": req.prompt})

    # 调用 AI（自动故障转移）
    content = await ai_manager.chat_with_failover(
        messages, max_tokens=req.max_tokens, temperature=req.temperature
    )

    # 缓存对话上下文到 Redis（TTL 30 分钟）
    if redis_client:
        ctx_key = f"ai:context:{hashlib.md5(req.prompt.encode()).hexdigest()[:8]}"
        await redis_client.set(ctx_key, json.dumps(messages), ex=1800)

    return ChatResponse(content=content)


@app.post("/ai/summarize")
async def summarize(req: SummarizeRequest):
    """离线消息摘要"""
    messages = [
        {"role": "system", "content": "请用一段话简要总结以下聊天记录的关键内容："},
        {"role": "user", "content": f"聊天记录：\n{req.messages}\n\n请提供摘要："},
    ]
    content = await ai_manager.chat_with_failover(messages, max_tokens=512)
    return {"summary": content}


@app.post("/ai/sentiment")
async def analyze_sentiment(req: SentimentRequest):
    """情感分析（返回 JSON 格式的情感分数）"""
    messages = [
        {"role": "system", "content": "分析以下文本的情感倾向，返回JSON格式：{\"sentiment\": \"positive/neutral/negative\", \"score\": 0.0-1.0}"},
        {"role": "user", "content": req.text},
    ]
    content = await ai_manager.chat_with_failover(messages, max_tokens=128)
    try:
        result = json.loads(content)
    except json.JSONDecodeError:
        result = {"sentiment": "neutral", "score": 0.5}
    return result


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
