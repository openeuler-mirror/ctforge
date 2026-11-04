// Package tlscfg 提供控制通道 TLS 配置。
// 本期采用「服务端自签证书 + 客户端指纹固定（trust on first use）」模型：
// 服务端首次启动自动生成 ECDSA P-256 自签证书存到磁盘；
// agent 端配置 server_fingerprint（证书 SHA-256 指纹）后做 pinning 校验，
// 从而免 CA 也能防中间人。
package tlscfg

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/hex"
	"encoding/pem"
	"fmt"
	"math/big"
	"net"
	"os"
	"time"
)

// EnsureCert 确保证书存在：certPath/keyPath 都存在则加载；否则生成自签证书并落盘。
// 返回 tls.Config 与证书 SHA-256 指纹（十六进制，用于下发给 agent 做 pinning）。
func EnsureCert(certPath, keyPath string, hosts []string) (*tls.Config, string, error) {
	if _, err := os.Stat(certPath); err == nil {
		if _, err := os.Stat(keyPath); err == nil {
			cfg, fp, err := loadCert(certPath, keyPath)
			if err == nil {
				return cfg, fp, nil
			}
		}
	}
	return genCert(certPath, keyPath, hosts)
}

func loadCert(certPath, keyPath string) (*tls.Config, string, error) {
	cert, err := tls.LoadX509KeyPair(certPath, keyPath)
	if err != nil {
		return nil, "", err
	}
	fp, err := FingerprintFromPEMFile(certPath)
	if err != nil {
		return nil, "", err
	}
	return &tls.Config{
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS12,
	}, fp, nil
}

func genCert(certPath, keyPath string, hosts []string) (*tls.Config, string, error) {
	priv, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, "", err
	}
	serial, _ := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	tmpl := x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "ctforge-server", Organization: []string{"ctforge"}},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().AddDate(10, 0, 0),
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
	}
	if len(hosts) == 0 {
		hosts = []string{"localhost", "127.0.0.1", "::1"}
	}
	for _, h := range hosts {
		if ip := net.ParseIP(h); ip != nil {
			tmpl.IPAddresses = append(tmpl.IPAddresses, ip)
		} else {
			tmpl.DNSNames = append(tmpl.DNSNames, h)
		}
	}
	der, err := x509.CreateCertificate(rand.Reader, &tmpl, &tmpl, &priv.PublicKey, priv)
	if err != nil {
		return nil, "", err
	}
	certOut, err := os.OpenFile(certPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return nil, "", err
	}
	if err := pem.Encode(certOut, &pem.Block{Type: "CERTIFICATE", Bytes: der}); err != nil {
		certOut.Close()
		return nil, "", err
	}
	certOut.Close()

	keyBytes, err := x509.MarshalECPrivateKey(priv)
	if err != nil {
		return nil, "", err
	}
	keyOut, err := os.OpenFile(keyPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0600)
	if err != nil {
		return nil, "", err
	}
	if err := pem.Encode(keyOut, &pem.Block{Type: "EC PRIVATE KEY", Bytes: keyBytes}); err != nil {
		keyOut.Close()
		return nil, "", err
	}
	keyOut.Close()

	return loadCert(certPath, keyPath)
}

// FingerprintFromPEMFile 计算 PEM 证书文件的 SHA-256 指纹（hex）。
func FingerprintFromPEMFile(path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	block, _ := pem.Decode(data)
	if block == nil {
		return "", fmt.Errorf("no PEM block in %s", path)
	}
	fp := sha256.Sum256(block.Bytes)
	return hex.EncodeToString(fp[:]), nil
}
