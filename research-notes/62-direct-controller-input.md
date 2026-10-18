# Direct controller input bring-up

The older request table in note 27 conflates a property with an outer request.
A bounded write trace of stock `syncboss_init` / `syncboss_input_start` on the current
Quest firmware establishes:

- Input mode uses outer request `0x03`, payload `d5 00` (one zero mode byte).
- Starting the normal radio manager additionally sends outer request `0x85`, empty payload.
- Sending bare request `0xd5`, or the input-mode property alone, did not produce controller
  packets after handing hardware ownership away from the stock sensor service.
- With both operations, a twelve-second open-camera session received 12,038 controller
  packets, alongside 12,001 head IMU packets and all four camera streams.

These are temporary session commands; they do not pair/unpair controllers or write firmware.
The stock trace also queries controller metadata and sends per-controller configuration.
Those later operations have not yet been fully reimplemented. Do not treat radio packets
alone as a successful end-to-end live pose test.

The diagnostic trace contains per-unit device identifiers and remains private. No stock
binary, calibration, or trace is included here.
