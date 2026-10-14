import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:yx_gateway_timer/presentation/device_scan/device_scan_page.dart';
import 'package:yx_gateway_timer/presentation/design/app_design.dart';
import 'package:yx_gateway_timer/presentation/providers.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const ProviderScope(child: YxGatewayApp()));
}

class YxGatewayApp extends ConsumerStatefulWidget {
  const YxGatewayApp({super.key});

  @override
  ConsumerState<YxGatewayApp> createState() => _YxGatewayAppState();
}

class _YxGatewayAppState extends ConsumerState<YxGatewayApp>
    with WidgetsBindingObserver {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      unawaited(ref.read(gatewayControllerProvider.notifier).onAppResumed());
    } else if (state == AppLifecycleState.paused ||
        state == AppLifecycleState.detached) {
      unawaited(ref.read(gatewayControllerProvider.notifier).onAppPaused());
    }
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'YX Gateway',
      theme: buildAppTheme(),
      home: const DeviceScanPage(),
    );
  }
}
