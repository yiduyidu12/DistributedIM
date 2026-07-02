export interface WebSocketMessage {
  type: string
  [key: string]: unknown
}

export interface LoginMessage extends WebSocketMessage {
  type: 'login'
  username: string
}

export interface ChatMessage extends WebSocketMessage {
  type: 'chat'
  msg: string
  msg_id?: string
  reply_to?: string
}

export interface SendMessage extends WebSocketMessage {
  type: 'send'
  to: string
  msg: string
  msg_id?: string
}

export interface GroupSendMessage extends WebSocketMessage {
  type: 'group_send'
  group_id: string
  msg: string
  msg_id?: string
}

export interface GroupCreateMessage extends WebSocketMessage {
  type: 'group_create'
  group_id: string
  group_name: string
  members: string[]
}

export interface GroupJoinMessage extends WebSocketMessage {
  type: 'group_join'
  group_id: string
}

export interface GroupLeaveMessage extends WebSocketMessage {
  type: 'group_leave'
  group_id: string
}

export interface GroupListMessage extends WebSocketMessage {
  type: 'group_list'
}

export interface AIMessageRequest extends WebSocketMessage {
  type: 'ai_chat' | 'ai_summary' | 'ai_analyze'
  prompt?: string
  context?: string
  messages?: string[]
  text?: string
}

export interface PingMessage extends WebSocketMessage {
  type: 'ping'
}

export interface E2EEKeyExchangeMessage extends WebSocketMessage {
  type: 'e2ee_key_exchange'
  to: string
  public_key: string
}