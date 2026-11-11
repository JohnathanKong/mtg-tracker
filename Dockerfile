FROM python:3.12-slim
ENV PIP_DISABLE_PIP_VERSION_CHECK=1
RUN pip install --no-cache-dir "platformio>=6.1,<7"
WORKDIR /src
CMD ["sh", "-c", "pio run -e waveshare-knob-s3 && mkdir -p build && cp .pio/build/waveshare-knob-s3/firmware.bin build/firmware.bin"]
