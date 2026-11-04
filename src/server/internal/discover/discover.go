// Package discover 实现服务端侧局域网发现与扫描应答。
//
// 两个方向：
//  1. agent 广播 → 服务端记录"局域网内有哪些 agent"（待接入展示）
//  2. agent 主动 scan_req → 服务端回 scan_resp（含 port + server_id），
//     供 agent 扫描服务端并据此发起握手。
package discover

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"log"
	"net"
	"time"

	"ctforge/server/internal/proto"
	"ctforge/server/internal/store"
)

const DefaultPort = 9001
const discoverMagic = "CTC1"

// Info 服务端应答扫描所需的信息
type Info struct {
	ServerID   string
	ServerName string
	Port       int // agent 控制通道端口
}

// Listen 启动 UDP 监听。ctx 取消时退出。
func Listen(ctx context.Context, st *store.Store, port int, info Info) error {
	addr := &net.UDPAddr{IP: net.IPv4zero, Port: port}
	conn, err := net.ListenUDP("udp4", addr)
	if err != nil {
		return err
	}
	log.Printf("[discover] udp listening on :%d (server_id=%s)", port, info.ServerID)

	go func() {
		<-ctx.Done()
		_ = conn.Close()
	}()

	// 定期清理过期的 discovered 记录
	go func() {
		ticker := time.NewTicker(60 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				st.PruneDiscovered(3 * time.Minute)
			}
		}
	}()

	buf := make([]byte, 4096)
	for {
		n, src, err := conn.ReadFromUDP(buf)
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			continue
		}
		handlePacket(conn, st, src, buf[:n], info)
	}
}

// 报文格式: 4B 魔数 "CTC1" + 2B BE 长度 + JSON
func handlePacket(conn *net.UDPConn, st *store.Store, src *net.UDPAddr, pkt []byte, info Info) {
	if len(pkt) < 6 || string(pkt[:4]) != discoverMagic {
		return
	}
	n := int(binary.BigEndian.Uint16(pkt[4:6]))
	if n <= 0 || n > len(pkt)-6 {
		return
	}
	body := pkt[6 : 6+n]

	// 先探测是否为 scan_req（agent 主动找服务端）
	var probe struct {
		Type string `json:"type"`
	}
	if err := json.Unmarshal(body, &probe); err == nil && probe.Type == "scan_req" {
		replyScan(conn, src, info)
		return
	}

	// 否则按 agent 广播处理（记录待接入主机）
	var d proto.DiscoverPayload
	if err := json.Unmarshal(body, &d); err != nil {
		return
	}
	if d.AgentID == "" {
		d.AgentID = d.Hostname
	}
	if d.AgentID == "" {
		return
	}
	if d.IP == "" {
		d.IP = src.IP.String()
	}
	st.Discover(&store.Agent{
		AgentID:     d.AgentID,
		Name:        d.Name,
		Hostname:    d.Hostname,
		IP:          d.IP,
		OS:          d.OS,
		OSVersion:   d.OSVersion,
		Kernel:      d.Kernel,
		Arch:        d.Arch,
		Version:     d.Version,
		DiskPercent: d.DiskPercent,
	})
}

// replyScan 回应 agent 的服务端扫描请求
func replyScan(conn *net.UDPConn, dst *net.UDPAddr, info Info) {
	body, err := json.Marshal(map[string]any{
		"type":        "scan_resp",
		"port":        info.Port,
		"server_id":   info.ServerID,
		"server_name": info.ServerName,
	})
	if err != nil {
		return
	}
	pkt := make([]byte, 6+len(body))
	copy(pkt, discoverMagic)
	binary.BigEndian.PutUint16(pkt[4:6], uint16(len(body)))
	copy(pkt[6:], body)
	_, _ = conn.WriteToUDP(pkt, dst)
}

// EncodeBeacon 供测试使用
func EncodeBeacon(d *proto.DiscoverPayload) ([]byte, error) {
	b, err := json.Marshal(d)
	if err != nil {
		return nil, err
	}
	if len(b) > 65507-6 {
		return nil, io.ErrShortBuffer
	}
	out := make([]byte, 6+len(b))
	copy(out, discoverMagic)
	binary.BigEndian.PutUint16(out[4:6], uint16(len(b)))
	copy(out[6:], b)
	return out, nil
}
