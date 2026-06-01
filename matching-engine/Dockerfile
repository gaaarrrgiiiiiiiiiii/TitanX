FROM eclipse-temurin:21-jre-alpine

WORKDIR /app

# Copy the shaded JAR built by maven-shade-plugin
COPY target/titanx-engine.jar /app/titanx-engine.jar

# JVM tuning for low-latency matching
ENV JVM_OPTS="-server -XX:+UseZGC -XX:MaxGCPauseMillis=1 -Xms512m -Xmx1g"

EXPOSE 8080

ENTRYPOINT ["sh", "-c", "java $JVM_OPTS -jar /app/titanx-engine.jar"]
