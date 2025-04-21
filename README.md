# dart-bridge

 Send data from Python to Dart via SendPort.

## Build/install package

```
pip install -e .
```

## Initialize 

## Set port from Dart

```
final setPort = nativeLib
    .lookupFunction<Void Function(Int64), void Function(int)>('set_dart_send_port_from_dart');

setPort(mySendPort.nativePort);
```

