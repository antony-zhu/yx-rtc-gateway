# YMZL/GS200 协议辅助库。
#
# 使用方式：把本文件随配置包一起下发，main.py 中 `import ymzl`。
# 本文件只依赖固件内置的 `ch37x` 原始字节流模块，不依赖设备管理 MQTT。

import ch37x


HEAD = b"\x55\xaa"
CMD_RECV = 0x12
CMD_SEND = 0x10
MAX_DATA_LEN = 34
MAX_FRAME_LEN = MAX_DATA_LEN + 6
CMD12_FIXED_LEN = 15

LIGHT_DEFAULT_GATEWAY_ADDR = 0x01FE
LIGHT_DEFAULT_CTRL = 0x80
LIGHT_OPCODE = 0x4D
LIGHT_SUB_OPCODE = 0x12
LIGHT_PARAM_ON = 0x03
LIGHT_PARAM_OFF = 0x01


def _crc_for_frame(data):
    """计算帧尾使用的 CRC16，返回高字节在前的 16 位整数。"""
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
            crc &= 0xFFFF
    return ((crc >> 8) | ((crc & 0xFF) << 8)) & 0xFFFF


def _read_be16(data, offset):
    return ((data[offset] << 8) | data[offset + 1]) & 0xFFFF


def build_frame(cmd, data=b""):
    """构建完整原始帧。data 最大 34 字节。"""
    payload = bytes(data)
    if len(payload) > MAX_DATA_LEN:
        raise ValueError("ymzl data too large")
    body = bytes((cmd & 0xFF, len(payload))) + payload
    crc = _crc_for_frame(body)
    return HEAD + body + bytes(((crc >> 8) & 0xFF, crc & 0xFF))


def parse_frame(frame):
    """校验并解析完整帧，返回 dict；raw/data 均为 bytes。"""
    raw = bytes(frame)
    if len(raw) < 6 or len(raw) > MAX_FRAME_LEN:
        raise ValueError("invalid ymzl frame length")
    if raw[0:2] != HEAD:
        raise ValueError("invalid ymzl frame head")
    data_len = raw[3]
    expected_len = data_len + 6
    if data_len > MAX_DATA_LEN or expected_len != len(raw):
        raise ValueError("invalid ymzl data length")
    crc_read = _read_be16(raw, len(raw) - 2)
    crc_calc = _crc_for_frame(raw[2 : 4 + data_len])
    if crc_read != crc_calc:
        raise ValueError("invalid ymzl crc")

    data = raw[4 : 4 + data_len]
    info = {
        "raw": raw,
        "cmd": raw[2],
        "data_len": data_len,
        "data": data,
        "ext_type": None,
    }
    if raw[2] == CMD_RECV and data_len >= CMD12_FIXED_LEN:
        op = data[CMD12_FIXED_LEN:]
        info["ext_type"] = "cmd12"
        info["cmd12"] = {
            "signal_strength": data[0],
            "device_id": data[1:7],
            "control": data[7],
            "seq": data[8],
            "device_addr": _read_be16(data, 9),
            "publish_addr": _read_be16(data, 11),
            "area_addr": _read_be16(data, 13),
            "operation_cmd": op[0] if len(op) > 0 else 0,
            "operation_sub_cmd": op[1] if len(op) > 1 else 0,
            "operation_raw": op,
        }
    return info


def build_light_control(area, address, onoff):
    """构建旧 GS200 常用灯控帧。"""
    data = bytearray(CMD12_FIXED_LEN + 3)
    data[7] = LIGHT_DEFAULT_CTRL
    data[8] = 0
    data[9] = (LIGHT_DEFAULT_GATEWAY_ADDR >> 8) & 0xFF
    data[10] = LIGHT_DEFAULT_GATEWAY_ADDR & 0xFF
    data[11] = (address >> 8) & 0xFF
    data[12] = address & 0xFF
    data[13] = (area >> 8) & 0xFF
    data[14] = area & 0xFF
    data[15] = 0x80 | (LIGHT_OPCODE & 0x7F)
    data[16] = LIGHT_SUB_OPCODE
    data[17] = LIGHT_PARAM_ON if onoff else LIGHT_PARAM_OFF
    return build_frame(CMD_SEND, data)


class Parser:
    """流式解析器，feed() 返回本次解析出的完整帧列表。"""

    def __init__(self):
        self._buf = bytearray()
        self.errors = []

    def reset(self):
        self._buf = bytearray()
        self.errors = []

    def _report_error(self, reason, dropped):
        self.errors.append({"reason": reason, "dropped": bytes(dropped)})
        if len(self.errors) > 16:
            self.errors.pop(0)

    def _drop(self, count):
        if count <= 0:
            return
        self._buf = self._buf[count:]

    def feed(self, data):
        frames = []
        self._buf.extend(data)

        while len(self._buf) >= 2:
            head = self._buf.find(HEAD)
            if head < 0:
                keep = self._buf[-1:] if self._buf[-1] == HEAD[0] else b""
                dropped = self._buf[: len(self._buf) - len(keep)]
                if dropped:
                    self._report_error("bad_head", dropped)
                self._buf = bytearray(keep)
                break

            if head > 0:
                self._report_error("bad_head", self._buf[:head])
                self._drop(head)

            if len(self._buf) < 4:
                break

            data_len = self._buf[3]
            frame_len = data_len + 6
            if data_len > MAX_DATA_LEN or frame_len > MAX_FRAME_LEN:
                self._report_error("bad_length", self._buf[:1])
                self._drop(1)
                continue

            if len(self._buf) < frame_len:
                break

            raw = bytes(self._buf[:frame_len])
            try:
                frames.append(parse_frame(raw))
                self._drop(frame_len)
            except ValueError:
                self._report_error("bad_crc", raw)
                self._drop(1)

        return frames


class YMZL:
    """CH37X + YMZL 组合封装，适合单脚本模型直接使用。"""

    def __init__(self, interface=ch37x.DEFAULT_INTERFACE):
        self.interface = interface
        self.parser = Parser()

    def start(self):
        ch37x.configure(self.interface)
        ch37x.start()

    def stop(self):
        ch37x.stop()

    def info(self):
        return ch37x.info()

    def pending(self):
        return ch37x.pending()

    def is_ready(self):
        return ch37x.is_ready()

    def disconnect(self):
        ch37x.disconnect()

    def read_frames(self, size=64, timeout_ms=0):
        data = ch37x.read(size, timeout_ms)
        if not data:
            return []
        return self.parser.feed(data)

    def send_raw(self, frame, timeout_ms=1000):
        return ch37x.write(frame, timeout_ms)

    def send_light_control(self, area, address, onoff, timeout_ms=1000):
        return self.send_raw(build_light_control(area, address, onoff), timeout_ms)
