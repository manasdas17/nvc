entity b is
end entity;

architecture a of b is
    type my_int1 is range 0 to 10;
    type my_int2 is range -20 to 30;
    signal x : my_int1 := 2;
begin

    process is
        variable z : my_int1;
    begin
        z := x;
    end process;
    
    process is
        variable y : my_int2;
    begin
        -- Should generate error as my_int1 and my_int2 incompatible
        y := x;
    end process;

    process is
        subtype my_int2_sub is my_int2 range 0 to 10;
        variable yy  : my_int2_sub;
    begin
        yy := 6;                        -- OK
        
        -- Should fail even though the range is the same
        yy := x;
    end process;

    process is
        -- Base type is undefined
        subtype bad is nothing range 1 to 2;
    begin
    end process;

    process is
        subtype my_int2_same is my_int2;
        subtype another_one is my_int2_same range 0 to 10;
        variable yyy : another_one;
        variable foo : my_int2 range 0 to 10;
    begin
        yyy := foo;                     -- OK
    end process;
    
end architecture;