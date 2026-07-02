<script setup lang="ts">
import { ref, watch, nextTick } from 'vue'
import { useMessageStore } from '@/stores/message'
import { useWebSocketStore } from '@/stores/websocket'
import { useUserStore } from '@/stores/user'

const props = defineProps<{
  groupId: string
  groupName: string
}>()

const messageStore = useMessageStore()
const wsStore = useWebSocketStore()
const userStore = useUserStore()

const newMessage = ref('')
const messages = ref<{ from: string; content: string; timestamp: number }[]>([])

watch(() => props.groupId, () => {
  loadMessages()
}, { immediate: true })

function loadMessages() {
  const groupMessages = messageStore.getMessagesByGroup(props.groupId)
  messages.value = groupMessages.map(m => ({
    from: m.from,
    content: m.content,
    timestamp: m.timestamp
  }))
}

async function sendMessage() {
  if (!newMessage.value.trim()) return

  const msgId = Date.now().toString()
  
  wsStore.send(JSON.stringify({
    type: 'group_send',
    group_id: props.groupId,
    msg: newMessage.value.trim(),
    msg_id: msgId
  }))

  messages.value.push({
    from: userStore.currentUser?.username || 'me',
    content: newMessage.value.trim(),
    timestamp: Date.now()
  })

  newMessage.value = ''
  await nextTick()
  scrollToBottom()
}

function scrollToBottom() {
  const container = document.querySelector('.group-messages-container')
  if (container) {
    container.scrollTop = container.scrollHeight
  }
}

function formatTime(timestamp: number): string {
  const date = new Date(timestamp * 1000)
  return `${date.getHours().toString().padStart(2, '0')}:${date.getMinutes().toString().padStart(2, '0')}`
}
</script>

<template>
  <div class="group-chat-container">
    <div class="group-chat-header">
      <h2>{{ groupName }}</h2>
      <span class="group-id">{{ groupId }}</span>
    </div>
    <div class="group-messages-container">
      <div
        v-for="(msg, index) in messages"
        :key="index"
        class="group-message"
        :class="{ 'sent': msg.from === userStore.currentUser?.username }"
      >
        <div class="message-sender">{{ msg.from }}</div>
        <div class="message-content">{{ msg.content }}</div>
        <div class="message-time">{{ formatTime(msg.timestamp) }}</div>
      </div>
    </div>
    <div class="input-container">
      <input
        v-model="newMessage"
        type="text"
        placeholder="输入群消息..."
        @keyup.enter="sendMessage"
      />
      <button @click="sendMessage">发送</button>
    </div>
  </div>
</template>

<style scoped>
.group-chat-container {
  flex: 1;
  display: flex;
  flex-direction: column;
  height: 100%;
  background: #f5f5f5;
}

.group-chat-header {
  background: white;
  padding: 16px;
  border-bottom: 1px solid #eee;
  display: flex;
  align-items: center;
  gap: 12px;
}

.group-chat-header h2 {
  margin: 0;
  color: #333;
  font-size: 18px;
}

.group-id {
  color: #9ca3af;
  font-size: 12px;
  background: #f5f5f5;
  padding: 4px 8px;
  border-radius: 4px;
}

.group-messages-container {
  flex: 1;
  overflow-y: auto;
  padding: 16px;
}

.group-message {
  max-width: 70%;
  margin-bottom: 16px;
  padding: 10px 14px;
  border-radius: 16px;
}

.group-message.sent {
  background: #667eea;
  color: white;
  margin-left: auto;
  border-bottom-right-radius: 4px;
}

.group-message:not(.sent) {
  background: white;
  color: #333;
  border-bottom-left-radius: 4px;
}

.message-sender {
  font-size: 12px;
  font-weight: 600;
  margin-bottom: 4px;
}

.group-message.sent .message-sender {
  color: rgba(255, 255, 255, 0.9);
}

.group-message:not(.sent) .message-sender {
  color: #667eea;
}

.message-content {
  font-size: 15px;
  line-height: 1.4;
}

.message-time {
  font-size: 11px;
  margin-top: 4px;
  opacity: 0.7;
}

.input-container {
  display: flex;
  gap: 8px;
  padding: 16px;
  background: white;
  border-top: 1px solid #eee;
}

.input-container input {
  flex: 1;
  padding: 12px 16px;
  border: 1px solid #eee;
  border-radius: 24px;
  font-size: 15px;
}

.input-container input:focus {
  outline: none;
  border-color: #667eea;
}

.input-container button {
  padding: 12px 24px;
  background: #667eea;
  color: white;
  border: none;
  border-radius: 24px;
  cursor: pointer;
}

.input-container button:hover {
  background: #5a6fd6;
}
</style>