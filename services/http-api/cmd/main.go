// DistributedIM - HTTP API 服务 (Go/Gin)
// 负责用户注册/登录、JWT 令牌管理、消息历史、审计日志接收
// 数据库：PostgreSQL（持久化存储）+ Redis（实时状态）

package main

import (
	"database/sql"
	"fmt"
	"log"
	"net/http"
	"os"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/golang-jwt/jwt/v5"
	"github.com/redis/go-redis/v9"
	"golang.org/x/crypto/bcrypt"
	_ "github.com/lib/pq"
)

var (
	db    *sql.DB
	rdb   *redis.Client
	jwtSecret = []byte(getEnv("JWT_SECRET", "change-me-in-production"))
)

func getEnv(key, defaultVal string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return defaultVal
}

func main() {
	// 连接 PostgreSQL
	var err error
	dsn := fmt.Sprintf("host=%s port=%s user=%s password=%s dbname=%s sslmode=disable",
		getEnv("DB_HOST", "localhost"),
		getEnv("DB_PORT", "5432"),
		getEnv("DB_USER", "distim"),
		getEnv("DB_PASSWORD", "distim_secret"),
		getEnv("DB_NAME", "distributed_im"),
	)
	db, err = sql.Open("postgres", dsn)
	if err != nil {
		log.Fatalf("Failed to connect to PostgreSQL: %v", err)
	}
	defer db.Close()

	// 连接 Redis
	rdb = redis.NewClient(&redis.Options{
		Addr: fmt.Sprintf("%s:%s", getEnv("REDIS_HOST", "localhost"), getEnv("REDIS_PORT", "6379")),
	})
	defer rdb.Close()

	// 自动迁移数据库表
	initDB()

	// 设置 Gin 路由
	r := gin.Default()

	// 公开 API
	r.POST("/api/register", handleRegister)
	r.POST("/api/login", handleLogin)
	r.POST("/api/refresh", handleRefreshToken)

	// 需要认证的 API
	auth := r.Group("/api")
	auth.Use(authMiddleware())
	{
		auth.GET("/profile", handleGetProfile)
		auth.PUT("/profile", handleUpdateProfile)
		auth.GET("/messages", handleGetMessages)
		auth.POST("/messages/search", handleSearchMessages)
		auth.DELETE("/messages/:id", handleDeleteMessage)
		auth.PUT("/messages/:id", handleEditMessage)
		auth.GET("/users/search", handleSearchUsers)
		auth.POST("/friends/add", handleAddFriend)
		auth.GET("/friends", handleGetFriends)
		auth.POST("/upload/avatar", handleUploadAvatar)
	}

	// 审计日志接收（内部 API）
	r.POST("/api/audit", handleAuditLog)

	// 健康检查
	r.GET("/health", func(c *gin.Context) { c.JSON(200, gin.H{"status": "ok"}) })

	// 启动服务
	port := getEnv("API_PORT", "8080")
	log.Printf("HTTP API Server starting on port %s", port)
	r.Run(":" + port)
}

// 数据库初始化
func initDB() {
	schema := `
	CREATE TABLE IF NOT EXISTS users (
		id SERIAL PRIMARY KEY,
		username VARCHAR(64) UNIQUE NOT NULL,
		password_hash VARCHAR(255) NOT NULL,
		nickname VARCHAR(128),
		avatar_url TEXT,
		status VARCHAR(32) DEFAULT 'offline',
		created_at TIMESTAMPTZ DEFAULT NOW(),
		updated_at TIMESTAMPTZ DEFAULT NOW()
	);

	CREATE TABLE IF NOT EXISTS messages (
		id SERIAL PRIMARY KEY,
		msg_id VARCHAR(64) UNIQUE NOT NULL,
		sender VARCHAR(64) NOT NULL,
		receiver VARCHAR(64),
		group_id VARCHAR(64),
		content TEXT NOT NULL,
		msg_type VARCHAR(32) NOT NULL DEFAULT 'text',
		is_deleted BOOLEAN DEFAULT FALSE,
		created_at TIMESTAMPTZ DEFAULT NOW()
	);
	CREATE INDEX IF NOT EXISTS idx_messages_sender ON messages(sender);
	CREATE INDEX IF NOT EXISTS idx_messages_receiver ON messages(receiver);
	CREATE INDEX IF NOT EXISTS idx_messages_created_at ON messages(created_at);
	CREATE INDEX IF NOT EXISTS idx_messages_content_fts ON messages USING gin(to_tsvector('simple', content));

	CREATE TABLE IF NOT EXISTS refresh_tokens (
		id SERIAL PRIMARY KEY,
		username VARCHAR(64) NOT NULL,
		token_hash VARCHAR(255) UNIQUE NOT NULL,
		expires_at TIMESTAMPTZ NOT NULL,
		created_at TIMESTAMPTZ DEFAULT NOW()
	);

	CREATE TABLE IF NOT EXISTS audit_logs (
		id SERIAL PRIMARY KEY,
		event_type VARCHAR(64) NOT NULL,
		username VARCHAR(64),
		target VARCHAR(128),
		detail JSONB,
		ip_address VARCHAR(45),
		created_at TIMESTAMPTZ DEFAULT NOW()
	);
	CREATE INDEX IF NOT EXISTS idx_audit_created_at ON audit_logs(created_at);
	`
	db.Exec(schema)
	log.Println("Database schema initialized")
}

// JWT 中间件
func authMiddleware() gin.HandlerFunc {
	return func(c *gin.Context) {
		tokenStr := c.GetHeader("Authorization")
		if tokenStr == "" || len(tokenStr) < 8 || tokenStr[:7] != "Bearer " {
			c.JSON(401, gin.H{"error": "unauthorized"})
			c.Abort()
			return
		}
		tokenStr = tokenStr[7:]

		token, err := jwt.Parse(tokenStr, func(t *jwt.Token) (interface{}, error) {
			return jwtSecret, nil
		})
		if err != nil || !token.Valid {
			c.JSON(401, gin.H{"error": "invalid token"})
			c.Abort()
			return
		}

		claims := token.Claims.(jwt.MapClaims)
		c.Set("username", claims["sub"])
		c.Next()
	}
}

// 用户注册
func handleRegister(c *gin.Context) {
	var req struct {
		Username string `json:"username" binding:"required,min=3,max=64"`
		Password string `json:"password" binding:"required,min=6"`
		Nickname string `json:"nickname"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(400, gin.H{"error": err.Error()})
		return
	}

	hash, err := bcrypt.GenerateFromPassword([]byte(req.Password), bcrypt.DefaultCost)
	if err != nil {
		c.JSON(500, gin.H{"error": "internal error"})
		return
	}

	_, err = db.Exec(
		"INSERT INTO users (username, password_hash, nickname) VALUES ($1, $2, $3)",
		req.Username, string(hash), req.Nickname,
	)
	if err != nil {
		c.JSON(409, gin.H{"error": "username already exists"})
		return
	}

	c.JSON(201, gin.H{"status": "ok"})
}

// 用户登录
func handleLogin(c *gin.Context) {
	var req struct {
		Username string `json:"username" binding:"required"`
		Password string `json:"password" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(400, gin.H{"error": err.Error()})
		return
	}

	var hash string
	err := db.QueryRow("SELECT password_hash FROM users WHERE username=$1", req.Username).Scan(&hash)
	if err != nil {
		c.JSON(401, gin.H{"error": "invalid credentials"})
		return
	}

	if bcrypt.CompareHashAndPassword([]byte(hash), []byte(req.Password)) != nil {
		c.JSON(401, gin.H{"error": "invalid credentials"})
		return
	}

	// 生成 JWT（15min access + 7day refresh）
	accessToken, _ := jwt.NewWithClaims(jwt.SigningMethodHS256, jwt.MapClaims{
		"sub": req.Username,
		"exp": time.Now().Add(15 * time.Minute).Unix(),
		"iat": time.Now().Unix(),
	}).SignedString(jwtSecret)

	refreshToken, _ := jwt.NewWithClaims(jwt.SigningMethodHS256, jwt.MapClaims{
		"sub": req.Username,
		"exp": time.Now().Add(7 * 24 * time.Hour).Unix(),
		"typ": "refresh",
	}).SignedString(jwtSecret)

	// 更新在线状态
	db.Exec("UPDATE users SET status='online' WHERE username=$1", req.Username)

	c.JSON(200, gin.H{
		"access_token":  accessToken,
		"refresh_token": refreshToken,
		"expires_in":    900,
	})
}

// Token 刷新
func handleRefreshToken(c *gin.Context) {
	var req struct {
		RefreshToken string `json:"refresh_token" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(400, gin.H{"error": err.Error()})
		return
	}

	token, err := jwt.Parse(req.RefreshToken, func(t *jwt.Token) (interface{}, error) {
		return jwtSecret, nil
	})
	if err != nil || !token.Valid {
		c.JSON(401, gin.H{"error": "invalid refresh token"})
		return
	}

	claims := token.Claims.(jwt.MapClaims)
	username := claims["sub"].(string)

	newAccessToken, _ := jwt.NewWithClaims(jwt.SigningMethodHS256, jwt.MapClaims{
		"sub": username,
		"exp": time.Now().Add(15 * time.Minute).Unix(),
		"iat": time.Now().Unix(),
	}).SignedString(jwtSecret)

	c.JSON(200, gin.H{"access_token": newAccessToken, "expires_in": 900})
}

func handleGetProfile(c *gin.Context) {
	username := c.GetString("username")
	var nick string
	var avatar sql.NullString
	var status string
	db.QueryRow("SELECT nickname, avatar_url, status FROM users WHERE username=$1", username).Scan(&nick, &avatar, &status)
	c.JSON(200, gin.H{"username": username, "nickname": nick, "avatar_url": avatar.String, "status": status})
}

func handleUpdateProfile(c *gin.Context) {
	var req struct {
		Nickname string `json:"nickname"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(400, gin.H{"error": err.Error()})
		return
	}
	username := c.GetString("username")
	db.Exec("UPDATE users SET nickname=$1 WHERE username=$2", req.Nickname, username)
	c.JSON(200, gin.H{"status": "ok"})
}

func handleGetMessages(c *gin.Context) {
	username := c.GetString("username")
	rows, _ := db.Query(
		`SELECT msg_id, sender, receiver, content, msg_type, created_at FROM messages
		 WHERE (sender=$1 OR receiver=$1) AND is_deleted=false
		 ORDER BY created_at DESC LIMIT 100`, username,
	)
	defer rows.Close()

	var msgs []gin.H
	for rows.Next() {
		var msgID, sender, receiver, content, msgType string
		var createdAt time.Time
		rows.Scan(&msgID, &sender, &receiver, &content, &msgType, &createdAt)
		msgs = append(msgs, gin.H{
			"msg_id": msgID, "sender": sender, "receiver": receiver,
			"content": content, "msg_type": msgType, "created_at": createdAt,
		})
	}
	c.JSON(200, msgs)
}

func handleSearchMessages(c *gin.Context) {
	var req struct {
		Query string `json:"query" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(400, gin.H{"error": err.Error()})
		return
	}
	username := c.GetString("username")
	rows, _ := db.Query(
		`SELECT msg_id, sender, receiver, content, msg_type, created_at FROM messages
		 WHERE to_tsvector('simple', content) @@ plainto_tsquery('simple', $1)
		 AND (sender=$2 OR receiver=$2) AND is_deleted=false LIMIT 50`,
		req.Query, username,
	)
	defer rows.Close()

	var msgs []gin.H
	for rows.Next() {
		var msgID, sender, receiver, content, msgType string
		var createdAt time.Time
		rows.Scan(&msgID, &sender, &receiver, &content, &msgType, &createdAt)
		msgs = append(msgs, gin.H{
			"msg_id": msgID, "sender": sender, "receiver": receiver,
			"content": content, "msg_type": msgType, "created_at": createdAt,
		})
	}
	c.JSON(200, msgs)
}

func handleDeleteMessage(c *gin.Context) {
	msgID := c.Param("id")
	username := c.GetString("username")
	db.Exec("UPDATE messages SET is_deleted=true WHERE msg_id=$1 AND sender=$2", msgID, username)
	c.JSON(200, gin.H{"status": "ok"})
}

func handleEditMessage(c *gin.Context) {
	var req struct {
		Content string `json:"content" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(400, gin.H{"error": err.Error()})
		return
	}
	msgID := c.Param("id")
	username := c.GetString("username")
	db.Exec("UPDATE messages SET content=$1 WHERE msg_id=$2 AND sender=$3", req.Content, msgID, username)
	c.JSON(200, gin.H{"status": "ok"})
}

func handleSearchUsers(c *gin.Context) {
	query := c.Query("q")
	rows, _ := db.Query("SELECT username, nickname, avatar_url, status FROM users WHERE username LIKE $1 LIMIT 20", "%"+query+"%")
	defer rows.Close()
	var users []gin.H
	for rows.Next() {
		var uname, nick string
		var avatar sql.NullString
		var status string
		rows.Scan(&uname, &nick, &avatar, &status)
		users = append(users, gin.H{"username": uname, "nickname": nick, "avatar_url": avatar.String, "status": status})
	}
	c.JSON(200, users)
}

func handleAddFriend(c *gin.Context) {
	var req struct {
		Username string `json:"username" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(400, gin.H{"error": err.Error()})
		return
	}
	currentUser := c.GetString("username")
	rdb.SAdd(c, "friends:"+currentUser, req.Username)
	c.JSON(200, gin.H{"status": "ok"})
}

func handleGetFriends(c *gin.Context) {
	username := c.GetString("username")
	friends, _ := rdb.SMembers(c, "friends:"+username).Result()
	c.JSON(200, gin.H{"friends": friends})
}

func handleUploadAvatar(c *gin.Context) {
	c.JSON(200, gin.H{"status": "ok", "msg": "avatar upload stub"})
}

func handleAuditLog(c *gin.Context) {
	var entries []map[string]interface{}
	if err := c.ShouldBindJSON(&entries); err != nil {
		c.JSON(400, gin.H{"error": err.Error()})
		return
	}
	for _, entry := range entries {
		db.Exec(
			"INSERT INTO audit_logs (event_type, username, target, detail, ip_address) VALUES ($1, $2, $3, $4, $5)",
			entry["event_type"], entry["username"], entry["target"],
			fmt.Sprintf("%v", entry["detail"]), entry["ip_address"],
		)
	}
	c.JSON(200, gin.H{"status": "ok", "count": len(entries)})
}
