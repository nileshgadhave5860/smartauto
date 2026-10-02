namespace Api.Module;

public sealed record MotorStatusResponse(int AutoId, bool MotorStatus, string Reason);