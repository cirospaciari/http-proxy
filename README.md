# http-proxy
Basic HTTP Proxy using custom [uWS](https://github.com/uNetworking/uWebSockets) for testing

## 📦 Installation
Binaries for macOS x64 & Silicon, Linux x64 on [bin](https://github.com/cirospaciari/http-proxy/tree/main/bin) folder

Dependencies
```bash
apt install libssl # or brew install openssl
apt install zlib1g # or brew install zlib
```

## 🤔 Usage
```bash
./http_proxy --port 8080 --host 0.0.0.0 --auth user:password --cert ./my_cert_file --key ./my_cert_key_file --passphrase "cert key pass" --logs
# if --cert and --key are present, run with TLS/HTTPS enabled if not run HTTP
# if --logs is present will show all logs, if not only listening logs
# if --port 0 is informed will pick a random available port
```

## :hammer: Building from source
```bash
# install dependencies
apt install libssl-dev # or brew install openssl
apt install zlib1g-dev # or brew install zlib

# clone and update submodules
git clone https://github.com/cirospaciari/http-proxy.git
git submodule update --init --recursive --remote
# build with make
make linux # or make macos or make macos-arm64
```
