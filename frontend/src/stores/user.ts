import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { User } from '@/types'

export const useUserStore = defineStore('user', () => {
  const currentUser = ref<User | null>(null)
  const onlineUsers = ref<User[]>([])

  function setCurrentUser(user: User) {
    currentUser.value = user
  }

  function clearCurrentUser() {
    currentUser.value = null
  }

  function setOnlineUsers(users: string[]) {
    onlineUsers.value = users.map(username => ({
      username,
      online: true
    }))
  }

  function updateUserOnlineStatus(username: string, online: boolean) {
    const user = onlineUsers.value.find(u => u.username === username)
    if (user) {
      user.online = online
    }
  }

  return {
    currentUser,
    onlineUsers,
    setCurrentUser,
    clearCurrentUser,
    setOnlineUsers,
    updateUserOnlineStatus
  }
})