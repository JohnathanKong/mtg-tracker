FROM python:3.12-slim
ENV PIP_DISABLE_PIP_VERSION_CHECK=1
RUN pip install --no-cache-dir "platformio>=6.1,<7"

# Copy and normalize shell entrypoint to avoid CRLF errors from Windows Git.
COPY scripts/create-factory-bin.sh /opt/mtg/create-factory-bin.sh
RUN sed -i 's/\r$//' /opt/mtg/create-factory-bin.sh

WORKDIR /src
CMD ["sh","-c","pio run -e waveshare-knob-s3 && mkdir -p build && cp .pio/build/waveshare-knob-s3/firmware.bin build/firmware.bin && sh /opt/mtg/create-factory-bin.sh"]
