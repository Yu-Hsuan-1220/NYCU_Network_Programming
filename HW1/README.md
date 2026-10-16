# HW1 — 1A2B over TCP

用 TCP socket 把單機版 1A2B 猜數字遊戲改寫成 client-server 版本。
完整規格見 [spec/NP_Project 1_Spec.pdf](spec/NP_Project%201_Spec.pdf)。

## 檔案結構

```
HW1/
├── spec/NP_Project 1_Spec.pdf   作業規格
├── src/
│   ├── Makefile
│   ├── nplab_server.cpp         iterative TCP server
│   └── nplab_client.cpp         TCP client
└── tests/test_nplab.py          端對端自動測試（不在繳交範圍）
```

## 編譯與執行

環境：Linux、g++（C++17）。在 `src/` 底下：

```bash
make
./nplab_server 1453               # port 範圍 1024–65535
./nplab_client 127.0.0.1 1453     # 也可以用 hostname，例如 localhost
```

```
Enter your name: Alice
Welcome, Alice!
nplab(Alice)> guess 6458
1A2B
nplab(Alice)> status
Guess count: 1, Correct count: 0
nplab(Alice)> quit
Guess count: 1, Correct count: 0
Goodbye, Alice!
```

指令：`guess <4-digit number>`、`status`、`quit`。猜中三次 `4A0B` 或輸入 `quit` 後遊戲結束。

## 設計

### Server：Iterative TCP Server

```
socket → setsockopt(SO_REUSEADDR) → bind(INADDR_ANY) → listen
loop:  accept → 服務一位玩家 → close → 回到 accept
```

- **一次服務一位玩家**：遊戲進行中，後來的連線停在 `listen()` 的 backlog 佇列，前一位離開後才會被 `accept()`。
- **Stateful**：連線期間保存玩家名稱、目前答案、猜測次數與猜中次數；斷線後清除。
- **TCP 沒有訊息邊界**：`LineReader` 會累積 `recv()` 收到的資料，湊到 `\n` 才當成一行處理。資料被切半、多行黏在一起或以 `\r\n` 結尾都能正確處理。
- **client 異常斷線時 server 不會終止**：忽略 `SIGPIPE`，並用 `send(..., MSG_NOSIGNAL)`。`recv()` 回傳 0 或 -1 時印出 `<name> left.`，接著等下一位。
- **輸出立即出現**：`setvbuf(stdout, nullptr, _IOLBF, 0)`，就算 stdout 接到 pipe 或檔案也一樣。
- **`SO_REUSEADDR`**：server 關閉後可以立刻重啟，不必等 TIME_WAIT 結束。

### Client：TCP client + `select()`

```
getaddrinfo → socket → connect → select 迴圈 → close
```

- 玩家看到的所有文字（包含提示字 `Enter your name: `、`nplab(<name>)> `）都由 server 送出，client 只負責轉送：鍵盤 → server、server → 螢幕。
- `select()` 同時等待 stdin 和 socket，server 關閉連線時 client 能立刻結束。
- 使用 `read()` / `write()` system call，不用 stdio，避免資料卡在 user-space buffer 而 `select()` 看不到。
- stdin 遇到 EOF（Ctrl+D，或輸入來自檔案或 pipe）時用 `shutdown(SHUT_WR)` 半關閉，仍能收到 server 最後的回覆。

### Protocol

純文字、以 `\n` 分行。client 送出使用者輸入的每一行；server 回覆結果與下一個提示字。

### Spec 未明定、自行決定的行為

- 空白行：重新顯示提示字。
- 未知指令只顯示第一個字：`foo bar` → `Unknown command: foo`。
- `status`、`quit` 後面多打的字會被忽略。
- 玩家輸入名稱前就斷線時，server 不印 `left.`，直接回去等待。

## 測試

[tests/test_nplab.py](tests/test_nplab.py) 會實際啟動 server 和 client，逐字比對輸出。編譯後執行：

```bash
python3 tests/test_nplab.py src
```

涵蓋項目：

- 參數錯誤時的 usage 訊息
- spec 範例的完整對話
- 不合法的名稱與指令
- 猜中三次自動結束
- client 被 kill、連線被 reset、stdin EOF 時 server 都能存活並繼續服務
- 第二位玩家排隊等待
- TCP 資料被切半或多行合併
- 以 hostname 連線
- A/B 計分
- `SO_REUSEADDR` 立即重啟

在 linux2.cs.nycu.edu.tw（Debian 12、g++ 12.2）上通過全部 30 項測試。
