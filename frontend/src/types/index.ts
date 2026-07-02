export interface User {
  username: string
  online: boolean
  gatewayId?: number
  lastSeen?: number
}

export interface Message {
  msgId: string
  type: string
  from: string
  to?: string
  groupId?: string
  content: string
  timestamp: number
  replyTo?: string
  reactions?: MessageReaction[]
}

export interface MessageReaction {
  msgId: string
  from: string
  reaction: string
}

export interface Group {
  groupId: string
  groupName: string
  creator: string
  members: string[]
  memberCount: number
}

export interface LoginResponse {
  type: string
  status: string
  gatewayId: number
}

export interface OnlineUsersResponse {
  type: string
  users: string[]
}

export interface ACKResponse {
  type: string
  msgId: string
  timestamp: number
}

export interface AIMessage {
  type: string
  content: string
  success: boolean
}

export interface GatewayInfo {
  gatewayId: string
  host: string
  port: number
  region: string
  load: number
}

export interface E2EEKeyExchange {
  type: string
  from: string
  to: string
  publicKey: string
  keyType: string
}