import Darwin
import Foundation

final class SerialPort {
    enum SerialError: LocalizedError {
        case cannotOpen(String)
        case cannotConfigure(String)

        var errorDescription: String? {
            switch self {
            case .cannotOpen(let path): "无法打开串口 \(path)"
            case .cannotConfigure(let reason): "无法配置串口：\(reason)"
            }
        }
    }

    private var descriptor: Int32 = -1
    private var source: DispatchSourceRead?
    var onReceive: (([UInt8]) -> Void)?
    var onDisconnect: ((Error) -> Void)?

    deinit { close() }

    func open(path: String, baudRate: Int) throws {
        close()
        descriptor = Darwin.open(path, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard descriptor >= 0 else { throw SerialError.cannotOpen(path) }
        do {
            try configure(baudRate: baudRate)
            let source = DispatchSource.makeReadSource(fileDescriptor: descriptor,
                                                        queue: .global(qos: .userInitiated))
            source.setEventHandler { [weak self] in self?.readAvailableBytes() }
            source.setCancelHandler { }
            self.source = source
            source.resume()
        } catch {
            close()
            throw error
        }
    }

    func close() {
        source?.cancel()
        source = nil
        if descriptor >= 0 {
            Darwin.close(descriptor)
            descriptor = -1
        }
    }

    static func availableDevices() -> [String] {
        guard let entries = try? FileManager.default.contentsOfDirectory(atPath: "/dev") else {
            return []
        }
        return entries
            .filter { $0.hasPrefix("tty.") || $0.hasPrefix("cu.") }
            .map { "/dev/\($0)" }
            .sorted { devicePriority($0) < devicePriority($1) }
    }

    private static func devicePriority(_ path: String) -> (Int, String) {
        if path.contains("tty.usb") { return (0, path) }
        if path.contains("cu.usb") { return (1, path) }
        if path.contains("tty.") { return (2, path) }
        return (3, path)
    }

    private func configure(baudRate: Int) throws {
        var options = termios()
        guard tcgetattr(descriptor, &options) == 0 else { throw SerialError.cannotConfigure(posixError) }
        cfmakeraw(&options)
        options.c_cflag |= tcflag_t(CLOCAL | CREAD)
        options.c_cflag &= ~tcflag_t(CSTOPB | PARENB | CRTSCTS)
        options.c_cflag &= ~tcflag_t(CSIZE)
        options.c_cflag |= tcflag_t(CS8)
        options.c_cc.16 = 0
        options.c_cc.17 = 0
        let speed = speed_t(baudRate)
        guard cfsetspeed(&options, speed) == 0, tcsetattr(descriptor, TCSANOW, &options) == 0 else {
            throw SerialError.cannotConfigure(posixError)
        }
    }

    private func readAvailableBytes() {
        var bytes = [UInt8](repeating: 0, count: 1_024)
        let count = bytes.withUnsafeMutableBytes { buffer in
            Darwin.read(descriptor, buffer.baseAddress, buffer.count)
        }
        if count > 0 {
            onReceive?(Array(bytes.prefix(Int(count))))
        } else if count < 0 && errno != EAGAIN {
            onDisconnect?(SerialError.cannotOpen("设备已断开"))
        }
    }

    private var posixError: String { String(cString: strerror(errno)) }
}
