import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { Message } from '@/types'

export const useMessageStore = defineStore('message', () => {
  const messages = ref<Message[]>([])
  const unreadCount = ref<Record<string, number>>({})

  function addMessage(message: Message) {
    messages.value.push(message)
    if (message.to) {
      unreadCount.value[message.to] = (unreadCount.value[message.to] || 0) + 1
    }
  }

  function addMessages(newMessages: Message[]) {
    messages.value = [...messages.value, ...newMessages]
  }

  function getMessagesByUser(username: string): Message[] {
    return messages.value.filter(
      m => m.from === username || m.to === username
    )
  }

  function getMessagesByGroup(groupId: string): Message[] {
    return messages.value.filter(m => m.groupId === groupId)
  }

  function markAsRead(username: string) {
    if (unreadCount.value[username]) {
      unreadCount.value[username] = 0
    }
  }

  function clearMessages() {
    messages.value = []
  }

  return {
    messages,
    unreadCount,
    addMessage,
    addMessages,
    getMessagesByUser,
    getMessagesByGroup,
    markAsRead,
    clearMessages
  }
})