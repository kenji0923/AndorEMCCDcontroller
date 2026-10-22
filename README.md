# AndorEMCCDcontroller

Andor iXon3 EMCCD（DU-897E-CS0-#BV、CCI-24 PCI カード）用のコントローラです。

- **バックエンド**（Linux、systemd サービス）：Andor SDK2 でカメラを制御し、設定・状態・画像を ZeroMQ で配信します。カメラの無い PC ではシミュレータ（`--sim`）で動きます。
- **フロントエンド**（Qt6 GUI、Linux / Windows）：ライブ表示（オートスケール・固定スケール、ズーム、カラーマップ）、設定の変更、ROI 演算（積分・最大・最小・平均・標準偏差）とその式、qtconsole 向けの UDP 送信、TIFF / PNG / FITS での保存。
- **Python クライアント**（`python/andor_emccd`）：labscript の BLACS worker などから、設定の読み書き、取得、画像の取得を行います。

詳しい説明（設計、インストール、実機での確認結果）は [doc/index.html](doc/index.html) にあります。

## 構成

```
src/            C++17 / CMake のソース（core: 共通ライブラリ、exec: backend と gui、test: GoogleTest）
python/         Python クライアント、labscript デバイス、試験スクリプト（bench/）
deploy/         バックエンドの systemd ユニット（システム / ユーザー）、インストーラ、udev ルール、設定例
tools/          Windows ビルドスクリプト、カメラ PC の準備スクリプト（camera_pc/）、補助スクリプト
doc/            説明書（HTML）
```

## 通信

| チャネル | 既定のエンドポイント | 内容 |
|---|---|---|
| control | `tcp://*:47100`、`ipc:///run/user/<uid>/aec/control` | 設定・状態・変更権（JSON） |
| data | `tcp://*:47101`、`ipc:///run/user/<uid>/aec/data` | 生画像（ヘッダ + 画素データ） |

同じ PC のクライアントは IPC を使い、ネットワークを経由しません。設定を変えられるのは変更権を持つクライアント 1 つだけです（他のクライアントからも見えます）。

## ビルド

### Linux（AlmaLinux 9 / 10）

```bash
sudo dnf install -y epel-release && sudo dnf config-manager --set-enabled crb
sudo dnf install -y gcc-c++ cmake ninja-build git cppzmq-devel zeromq-devel spdlog-devel \
    qt6-qtbase-devel libtiff-devel libpng-devel cfitsio-devel
cmake -S src -B build -G Ninja -DANDOR_SDK_ROOT=/usr/local
cmake --build build
ctest --test-dir build --output-on-failure
```

Andor SDK（`atmcdLXd.h`、`libandor.so`）が見つからないときは、シミュレータだけのバックエンドになります。SDK とドライバは Andor から入手してください（このリポジトリには含みません）。カメラ PC の準備は `tools/camera_pc/` と doc/index.html の 2 章を参照してください。ドライバのパッチは Andor のドライバソースの差分なので、このリポジトリには含めていません（2 章に変更内容を書いてあります）。

```bash
build/exec/andor_emccd_backend --sim                      # カメラなしで試す
build/exec/andor_emccd_backend --config deploy/backend.json
build/exec/gui/andor_emccd_gui --host <backend の PC> --name <名前>
```

### サービスとして動かす（カメラ PC）

システムサービスとユーザーサービスのどちらでも動かせます（同時にはどちらか一方。詳しくは doc/index.html の 2.3 節）。

```bash
# システムサービス（専用の andor ユーザーで実行。常設のカメラ PC 向け）
sudo AEC_FIREWALL_ZONE=internal deploy/install_backend.sh build
systemctl status andor-emccd-backend

# ユーザーサービス（ログインユーザーで実行。root は最初の 1 回だけ）
sudo deploy/prepare_user_service_root.sh        # andor グループ、udev、/run/andor-emccd、firewall、linger
deploy/install_backend_user.sh build            # 再ログイン後、ユーザーで。更新時も同じ
systemctl --user status andor-emccd-backend-user
```

### Windows（フロントエンドのみ）

Qt 6.8（aqtinstall）と vcpkg を `%USERPROFILE%\Opt` に入れ、`tools\build_windows.bat` を実行します。配布できる一式が `build-win\deploy` にできます（doc/index.html の 10 章）。

### GitHub Actions

`.github/workflows/build.yml` が push ごとにビルドとテストを行い、次のものを Artifacts に置きます。`v*` のタグを push すると Release にも添付します。

| 名前 | 内容 |
|---|---|
| `andor_emccd_gui-windows-x64` | Windows 版 GUI（exe と Qt / 依存 DLL の一式） |
| `andor_emccd_gui-linux-x86_64` | Linux 版 GUI の AppImage（AlmaLinux 9 でビルド。glibc 2.34 以降で動作） |

バックエンドは Andor SDK が必要なので、CI ではシミュレータ版のビルドとテストだけを行います。実機用はカメラ PC でビルドしてください。

## Python クライアント

```bash
pip install -e python            # numpy, pyzmq
```

```python
from andor_emccd import AndorEMCCDClient
cam = AndorEMCCDClient("camera-pc", name="worker", acquire=True)   # 変更権を取る
cam.set_settings(exposure_time_s=0.01, em_gain_enabled=True, em_gain=50)
images, headers = cam.acquire(10)                              # numpy (n, h, w)
cam.release_control()
```
