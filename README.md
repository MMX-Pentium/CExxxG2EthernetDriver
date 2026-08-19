# CE100G2 Ubuntu Server L2 スイッチドライバ

Trend Micro Cloud Edge CE100G2（Lanner NCA-2011Z-TM2J/TM2E/TM3J/TM3E）上の
Marvell 88E6190 を Ubuntu Server 24.04 から利用するための DKMS パッケージです。

古い製品 OS のカーネルモジュールを直接移植したものではありません。Ubuntu の
`ixgbe`、DSA、`mv88e6xxx` を基礎に、基板固有の配線登録、X553 PCS の2.5Gbps
初期化、およびLAN8/WAN間のハードウェア・バイパスリレー初期化を追加します。

## 対応状況

- Ubuntu Server 24.04 GA、カーネル 6.8.0-138 でコンパイル確認済み
- LAN ポートを `lan1` ～ `lan8` として登録
- DMI 製品名と PCI ID（Intel X553 `8086:15c2`）を照合して誤動作を防止
- 実機で 88E6190、PHY 1～8、DSA、`lan1`～`lan8` の登録を確認済み
- Ubuntu `6.8.0-100.100` の公式 ixgbe を基にしたパッチ済みモジュールを同梱
- 実機で両 X553 uplink の 2.5Gbps/full-duplex リンクアップを確認済み
- CPU側内蔵PHYが未初期化の個体でもPCS状態を検査し、必要な場合だけ再起動
- initramfs に DKMS 版 `ixgbe` を確実に収録する専用フックを同梱
- 筐体刻印 LAN1～LAN8 が Linux の `lan1`～`lan8` に対応することを確認済み
- LAN8/WANバイパスを通電中NICモードへ設定し、電源断時のfail-open設定は維持
- LAN2およびLAN8で1Gbps/full-duplex、DHCP取得、LAN内ゲートウェイ疎通を確認済み
- 独立I211の管理ポートとWANを起動し、WANでも1Gbps、DHCP、LAN内疎通を確認済み

## インストール

コンソール接続を維持した状態で実施してください。

```bash
sudo apt update
sudo apt install ./ce100g2-switch-dkms_0.1.10-1_all.deb
sudo reboot
sudo systemctl status ce100g2-switch.service
sudo ce100g2-switch-status
```

DKMS のビルドに対応するヘッダーがない場合は、先に次を実行します。

```bash
sudo apt install "linux-headers-$(uname -r)"
```

再起動は、既にロード済みの標準 `ixgbe` をパッチ済みモジュールへ確実に切り替える
ために必要です。0.1.8 以降はインストール時に initramfs も自動更新します。成功時は
`ip -br link` に `lan1` ～ `lan8` が現れます。IP アドレスは X553 の
親インターフェースではなく、使用する `lanN` またはそれらを収容する Linux bridge
へ設定してください。

## 問題発生時

```bash
sudo ce100g2-switch-status > ce100g2-switch-status.txt
sudo journalctl -u ce100g2-switch.service -b --no-pager
```

サービスの起動だけを止める場合:

```bash
sudo systemctl disable --now ce100g2-switch.service
```

完全に削除する場合:

```bash
sudo apt remove ce100g2-switch-dkms
sudo reboot
```

このパッケージは `ixgbe` もDKMSで置換します。アンインストール後の再起動で
Ubuntu標準モジュールへ戻ります。

詳細な解析根拠と既知の制約は [docs/解析レポート.md](docs/解析レポート.md) を参照してください。

## ライセンス

リポジトリ独自の文書・スクリプトはトップレベルの `LICENSE`（MIT）に従います。
カーネルモジュールと同梱 `ixgbe` ソースは各ファイルの SPDX 表記および
`debian/copyright` に従い、GPL-2.0 です。
