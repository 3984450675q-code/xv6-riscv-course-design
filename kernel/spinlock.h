// Mutual exclusion lock.
struct spinlock {
  uint locked; // Is the lock held?

  // For debugging:
  char *name;      // Name of lock.
  struct cpu *cpu; // The cpu holding the lock.
  uint n;          // Number of acquire attempts.
  uint nts;        // Number of failed test-and-set attempts.
};
