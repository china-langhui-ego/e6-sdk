#!/bin/bash
cd "$(dirname "$0")"

# ============================================================
# Parse --port from arguments
# ============================================================
PORT=9000
prev=""
for arg in "$@"; do
    case "$arg" in
        --port=*) PORT="${arg#*=}" ;;
        --port)    prev="--port" ;;
        *)
            [ "$prev" = "--port" ] && PORT="$arg"
            ;;
    esac
    [ "$arg" != "--port" ] && [ "${arg#--port=}" = "$arg" ] && prev=""
done

# ============================================================
# Firewall detection & auto-fix
# ============================================================

check_and_open_port() {
    local port=$1
    local fw_name=""

    # --- ufw ---
    if systemctl is-active --quiet ufw 2>/dev/null; then
        fw_name="ufw"

        # Already allowed?
        if /usr/sbin/ufw status 2>/dev/null | grep -q "$port/tcp.*ALLOW"; then
            return 0
        fi

        # Need root to read status — assume blocked
        echo ""
        echo "  ==============================================="
        echo "  防火墙 (ufw) 已启用，端口 $port 未放行"
        echo "  ==============================================="
        echo ""

        # Try non-interactive sudo first
        if sudo -n true 2>/dev/null; then
            echo "  → 自动添加防火墙规则..."
            if sudo -n /usr/sbin/ufw allow "$port/tcp" 2>/dev/null; then
                echo "  ✓ 端口 $port/tcp 已放行"
                echo ""
                return 0
            fi
        fi

        # Try interactive sudo (will prompt for password)
        echo "  → 需要 root 权限添加防火墙规则"
        echo ""
        if sudo /usr/sbin/ufw allow "$port/tcp" 2>/dev/null; then
            echo ""
            echo "  ✓ 端口 $port/tcp 已放行"
            echo ""
            return 0
        fi

        # Failed — show manual instructions
        echo ""
        echo "  ✗ 自动配置失败，请手动运行:"
        echo "      sudo ufw allow $port/tcp"
        echo ""
        echo "  或临时关闭防火墙测试:"
        echo "      sudo ufw disable"
        echo ""
        read -rp "  按 Enter 启动服务器（端口可能仍被拦截）... " _
        echo ""
        return 1
    fi

    # --- firewalld ---
    if systemctl is-active --quiet firewalld 2>/dev/null; then
        fw_name="firewalld"

        if firewall-cmd --list-ports 2>/dev/null | grep -q "$port/tcp"; then
            return 0
        fi

        echo ""
        echo "  ==============================================="
        echo "  防火墙 (firewalld) 已启用，端口 $port 未放行"
        echo "  ==============================================="
        echo ""

        if sudo -n true 2>/dev/null; then
            echo "  → 自动添加防火墙规则..."
            sudo -n firewall-cmd --add-port="$port/tcp" --permanent 2>/dev/null
            sudo -n firewall-cmd --reload 2>/dev/null
            if firewall-cmd --list-ports 2>/dev/null | grep -q "$port/tcp"; then
                echo "  ✓ 端口 $port/tcp 已放行"
                echo ""
                return 0
            fi
        fi

        echo "  → 需要 root 权限添加防火墙规则"
        echo ""
        if sudo firewall-cmd --add-port="$port/tcp" --permanent 2>/dev/null && \
           sudo firewall-cmd --reload 2>/dev/null; then
            echo ""
            echo "  ✓ 端口 $port/tcp 已放行"
            echo ""
            return 0
        fi

        echo ""
        echo "  ✗ 自动配置失败，请手动运行:"
        echo "      sudo firewall-cmd --add-port=$port/tcp --permanent"
        echo "      sudo firewall-cmd --reload"
        echo ""
        read -rp "  按 Enter 启动服务器（端口可能仍被拦截）... " _
        echo ""
        return 1
    fi

    # --- nftables/iptables (best-effort, no root access) ---
    if [ -f /etc/nftables.conf ] || [ -f /etc/iptables/rules.v4 ]; then
        fw_name="nftables/iptables"
        echo ""
        echo "  ==============================================="
        echo "  防火墙 ($fw_name) 已启用"
        echo "  端口 $port 需要手动放行"
        echo "  ==============================================="
        echo ""
        echo "  请手动执行:"
        echo "      sudo iptables -I INPUT -p tcp --dport $port -j ACCEPT"
        echo ""
        read -rp "  按 Enter 启动服务器（端口可能仍被拦截）... " _
        echo ""
        return 1
    fi

    # No firewall detected
    return 0
}

check_and_open_port "$PORT"

# ============================================================
# Start server
# ============================================================
python3 server.py "$@"
