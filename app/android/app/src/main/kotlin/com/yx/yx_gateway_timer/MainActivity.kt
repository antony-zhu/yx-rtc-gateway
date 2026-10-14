package com.yx.yx_gateway_timer

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import io.reactivex.exceptions.UndeliverableException
import io.reactivex.plugins.RxJavaPlugins

class MainActivity : FlutterActivity() {
    private val handler = Handler(Looper.getMainLooper())
    private var bondReceiver: BroadcastReceiver? = null
    private var pendingBondResult: MethodChannel.Result? = null
    private var bondTimeout: Runnable? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        installRxJavaErrorHandler()
        super.onCreate(savedInstanceState)
    }

    /**
     * 吸收 BLE 通知流取消后迟到的断线异常。
     *
     * flutter_reactive_ble 的 Android 实现基于 RxJava。连接断开与 Flutter
     * 取消 EventChannel 可能并发发生，此时 RxJava 会把已经无法交付的
     * BleDisconnectedException 送到全局错误处理器。该异常只表示已取消连接的
     * 收尾状态，不应导致整个 APP 进程崩溃；其他异常仍交给系统崩溃处理器。
     */
    private fun installRxJavaErrorHandler() {
        RxJavaPlugins.setErrorHandler { error ->
            val cause = if (error is UndeliverableException) error.cause ?: error else error
            val isCanceledBleDisconnect = error is UndeliverableException &&
                cause.javaClass.name.startsWith("com.polidea.rxandroidble2.exceptions.")
            if (isCanceledBleDisconnect) {
                Log.w(TAG, "忽略已取消 BLE 流的迟到异常: ${cause.javaClass.simpleName}")
                return@setErrorHandler
            }

            val thread = Thread.currentThread()
            val handler = thread.uncaughtExceptionHandler
            if (handler != null) {
                handler.uncaughtException(thread, cause)
            } else {
                Log.e(TAG, "未处理的 RxJava 异常", cause)
            }
        }
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(
            flutterEngine.dartExecutor.binaryMessenger,
            BOND_CHANNEL,
        ).setMethodCallHandler { call, result ->
            if (call.method != "waitForBond") {
                result.notImplemented()
                return@setMethodCallHandler
            }
            val address = call.argument<String>("deviceId")
            waitForBond(address, result)
        }
    }

    /**
     * 等待指定 BLE 外设完成系统配对。
     *
     * 仅用于厂商蓝牙栈已主动进入 BOND_BONDING、但服务发现提前返回失败的场景；
     * 未配对时会调用 Android 系统 createBond；不会访问或返回任何密钥材料。
     */
    @SuppressLint("MissingPermission")
    private fun waitForBond(address: String?, result: MethodChannel.Result) {
        if (address == null || !BluetoothAdapter.checkBluetoothAddress(address)) {
            result.error("invalid_device", "无效的 Android BLE 地址", null)
            return
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            result.error("permission_denied", "缺少附近设备连接权限", null)
            return
        }
        if (pendingBondResult != null) {
            result.error("bond_wait_busy", "已有配对等待正在进行", null)
            return
        }

        val manager = getSystemService(BluetoothManager::class.java)
        val adapter = manager?.adapter
        if (adapter == null) {
            result.error("bluetooth_unavailable", "蓝牙不可用", null)
            return
        }
        val device = adapter.getRemoteDevice(address)
        val shouldStartBond = device.bondState == BluetoothDevice.BOND_NONE
        when (device.bondState) {
            BluetoothDevice.BOND_BONDED -> {
                result.success(true)
                return
            }
        }

        pendingBondResult = result
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                val changedDevice = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    intent.getParcelableExtra(
                        BluetoothDevice.EXTRA_DEVICE,
                        BluetoothDevice::class.java,
                    )
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                }
                if (changedDevice?.address != address) return
                when (intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR)) {
                    BluetoothDevice.BOND_BONDED -> finishBondWait(true)
                    BluetoothDevice.BOND_NONE -> finishBondWait(false)
                }
            }
        }
        bondReceiver = receiver
        val filter = IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(receiver, filter)
        }
        bondTimeout = Runnable { finishBondWait(false) }.also {
            handler.postDelayed(it, BOND_TIMEOUT_MS)
        }
        if (shouldStartBond && !device.createBond()) {
            finishBondWait(false)
        }
    }

    private fun finishBondWait(bonded: Boolean) {
        bondTimeout?.let(handler::removeCallbacks)
        bondTimeout = null
        bondReceiver?.let {
            try {
                unregisterReceiver(it)
            } catch (_: IllegalArgumentException) {
                // Activity 销毁和广播回调并发时，接收器可能已经被注销。
            }
        }
        bondReceiver = null
        pendingBondResult?.success(bonded)
        pendingBondResult = null
    }

    override fun onDestroy() {
        finishBondWait(false)
        super.onDestroy()
    }

    private companion object {
        const val TAG = "YxGatewayMain"
        const val BOND_CHANNEL = "com.yx.yx_gateway_timer/bonding"
        const val BOND_TIMEOUT_MS = 60_000L
    }
}
